/**
 * @file serial_api.c
 * @brief Machine-readable "API mode" bridge over the serial console (Phase 1).
 *
 * The hosted web UI sends `api <id> <VERB> <path> [<base64-body>]` and gets back
 * `@RES <id> <status> <len> <base64-payload>`. Payloads are byte-for-byte the
 * same the old WiFi HTTP endpoints returned, so the existing app.js parsing is
 * reused unchanged. Implements: /ap-list, /status, /run-attack, /reset, /stop,
 * /ping.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_event.h"
#include "mbedtls/base64.h"

#include "serial_api.h"
#include "webserver.h"        /* WEBSERVER_EVENTS, attack_request_t */
#include "wifi_controller.h"  /* scan + AP records */
#include "attack.h"           /* attack_get_status */

/* ── Framed response helpers ─────────────────────────────────────────────── */
static void api_respond(const char *id, int status, const uint8_t *payload, size_t len) {
    if (len == 0 || payload == NULL) {
        printf("@RES %s %d 0 \n", id, status);
        fflush(stdout);
        return;
    }
    size_t cap = 4 * ((len + 2) / 3) + 1;
    unsigned char *b64 = malloc(cap);
    if (!b64) { printf("@RES %s 500 0 \n", id); fflush(stdout); return; }
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, cap, &olen, payload, len) != 0) {
        free(b64);
        printf("@RES %s 500 0 \n", id);
        fflush(stdout);
        return;
    }
    printf("@RES %s %d %u ", id, status, (unsigned) len);
    fwrite(b64, 1, olen, stdout);
    printf("\n");
    fflush(stdout);
    free(b64);
}

static void api_respond_text(const char *id, int status, const char *text) {
    api_respond(id, status, (const uint8_t *) text, text ? strlen(text) : 0);
}

/* ── Endpoint handlers (mirror the old HTTP handlers byte-for-byte) ───────── */

/* GET /ap-list — 40 bytes per AP: ssid[33] + bssid[6] + rssi[1] */
static void handle_ap_list(const char *id) {
    wifictl_scan_nearby_aps();
    const wifictl_ap_records_t *recs = wifictl_get_ap_records();
    size_t n = recs ? recs->count : 0;
    size_t len = n * 40;
    uint8_t *buf = len ? malloc(len) : NULL;
    if (len && !buf) { api_respond_text(id, 500, "oom"); return; }
    for (size_t i = 0; i < n; i++) {
        uint8_t *rec = buf + i * 40;
        memcpy(rec,       recs->records[i].ssid,  33);
        memcpy(rec + 33,  recs->records[i].bssid,  6);
        memcpy(rec + 39, &recs->records[i].rssi,   1);
    }
    api_respond(id, 200, buf, len);
    free(buf);
}

/* GET /status — 4-byte header (state,type,size LE) + content when finished */
static void handle_status(const char *id) {
    const attack_status_t *st = attack_get_status();
    size_t clen = 0;
    if ((st->state == FINISHED || st->state == TIMEOUT) && st->content_size > 0) {
        clen = st->content_size;
    }
    size_t len = 4 + clen;
    uint8_t *buf = malloc(len);
    if (!buf) { api_respond_text(id, 500, "oom"); return; }
    buf[0] = st->state;
    buf[1] = st->type;
    buf[2] = st->content_size & 0xff;
    buf[3] = (st->content_size >> 8) & 0xff;
    if (clen) memcpy(buf + 4, st->content, clen);
    api_respond(id, 200, buf, len);
    free(buf);
}

/* POST /run-attack — base64 of the 22-byte attack_request_t */
static void handle_run_attack(const char *id, const char *b64) {
    unsigned char raw[64];
    size_t olen = 0;
    if (!b64 ||
        mbedtls_base64_decode(raw, sizeof(raw), &olen, (const unsigned char *) b64, strlen(b64)) != 0 ||
        olen != sizeof(attack_request_t)) {
        api_respond_text(id, 400, "bad payload");
        return;
    }
    esp_event_post(WEBSERVER_EVENTS, WEBSERVER_EVENT_ATTACK_REQUEST,
                   raw, sizeof(attack_request_t), portMAX_DELAY);
    api_respond(id, 200, NULL, 0);
}

/* POST /reset — same as the old HEAD /reset (clears status) */
static void handle_reset(const char *id) {
    esp_event_post(WEBSERVER_EVENTS, WEBSERVER_EVENT_ATTACK_RESET, NULL, 0, portMAX_DELAY);
    api_respond(id, 200, NULL, 0);
}

/* POST /stop — actually abort whatever attack is running (per-type cleanup),
 * unlike /reset which only clears the displayed status. Only meaningful over
 * this USB channel: the old WiFi UI never had a live stop button because most
 * attacks killed the management AP the browser depended on. */
static void handle_stop(const char *id) {
    attack_stop_current();
    api_respond(id, 200, NULL, 0);
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */
static int cmd_api(int argc, char **argv) {
    if (argc < 4) { printf("@RES ? 400 0 \n"); fflush(stdout); return 0; }
    const char *id   = argv[1];
    const char *verb = argv[2];
    const char *path = argv[3];
    const char *body = (argc >= 5) ? argv[4] : NULL;

    if      (!strcmp(verb, "GET")  && !strcmp(path, "/ap-list"))    handle_ap_list(id);
    else if (!strcmp(verb, "GET")  && !strcmp(path, "/status"))     handle_status(id);
    else if (!strcmp(verb, "POST") && !strcmp(path, "/run-attack")) handle_run_attack(id, body);
    else if (!strcmp(path, "/reset"))                                handle_reset(id);
    else if (!strcmp(path, "/stop"))                                 handle_stop(id);
    else if (!strcmp(path, "/ping"))                                 api_respond_text(id, 200, "pong");
    else                                                             api_respond_text(id, 404, "unknown endpoint");
    return 0;
}

void serial_api_register(void) {
    const esp_console_cmd_t c = {
        .command = "api",
        .help = "crx3 web API bridge used by the hosted UI (api <id> <VERB> <path> [b64])",
        .hint = NULL,
        .func = cmd_api,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
