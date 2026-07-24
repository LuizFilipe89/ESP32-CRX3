/**
 * @file serial_api.c
 * @brief Machine-readable "API mode" bridge over the serial console.
 *
 * The hosted web UI sends `api <id> <VERB> <path> [<base64-body>]` and gets back
 * `@RES <id> <status> <len> <base64-payload>`. Payloads are byte-for-byte the
 * same the old WiFi HTTP endpoints returned, so the existing app.js parsing is
 * reused unchanged. Covers the full feature set: scan/attack/status, Evil Twin
 * (status, custom-name launch, captive portal upload/restore, credential log),
 * the deauth detector, settings, and the network printer tab.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"   /* httpd_query_key_value() — pure string utility, no live server needed */
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/base64.h"

#include "serial_api.h"
#include "webserver.h"          /* WEBSERVER_EVENTS, attack_request_t, PORTAL_* paths */
#include "wifi_controller.h"    /* scan + AP records */
#include "attack.h"             /* attack_get_status */
#include "attack_eviltwin.h"
#include "attack_deauth_detector.h"
#include "pcap_serializer.h"
#include "hccapx_serializer.h"
#include "printer.h"

/* ── Framed response helpers ─────────────────────────────────────────────── */
/* Writes the whole "@RES ..." line with ONE fwrite() instead of separate
 * printf()/fwrite()/printf() calls. Each stdio call here becomes an
 * immediate, separate UART write (this console isn't line-buffered), which
 * for a long base64 payload meant several small back-to-back USB bulk OUT
 * transfers with tiny gaps between them instead of one contiguous burst.
 * Jason2866/esp32tool's own Android WebUSB notes document exactly this
 * pattern (their stub-loader "0xC0 flush" bug) as a cause of Android
 * actually LOSING bulk transfers, not merely delaying them — independent of
 * anything the client can do to recover. One write closes that gap. */
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

    char header[48];
    int hlen = snprintf(header, sizeof(header), "@RES %s %d %u ", id, status, (unsigned) len);
    char *frame = malloc((size_t) hlen + olen + 1);
    if (!frame) {
        free(b64);
        printf("@RES %s 500 0 \n", id);
        fflush(stdout);
        return;
    }
    memcpy(frame, header, (size_t) hlen);
    memcpy(frame + hlen, b64, olen);
    frame[(size_t) hlen + olen] = '\n';
    fwrite(frame, 1, (size_t) hlen + olen + 1, stdout);
    fflush(stdout);
    free(frame);
    free(b64);
}

static void api_respond_text(const char *id, int status, const char *text) {
    api_respond(id, status, (const uint8_t *) text, text ? strlen(text) : 0);
}

/* Decodes a base64 request body into a NUL-terminated text buffer — for
 * form-encoded ("key=value&...") or plain-text bodies. Returns the decoded
 * length, or -1 if missing/invalid/too big for out_cap. */
static int decode_body_text(const char *b64, char *out, size_t out_cap) {
    if (!b64 || !b64[0]) { out[0] = '\0'; return 0; }
    size_t olen = 0;
    if (mbedtls_base64_decode((unsigned char *) out, out_cap - 1, &olen, (const unsigned char *) b64, strlen(b64)) != 0) {
        return -1;
    }
    out[olen] = '\0';
    return (int) olen;
}

/** Mirrors the tiny percent-decoder in webserver.c (not exposed there —
 *  duplicated rather than making it public API for a 15-line utility). */
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            isxdigit((unsigned char) a) && isxdigit((unsigned char) b)) {
            if (a >= 'a') a -= 32;
            a = (a >= 'A') ? a - 'A' + 10 : a - '0';
            if (b >= 'a') b -= 32;
            b = (b >= 'A') ? b - 'A' + 10 : b - '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
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
 * unlike /reset which only clears the displayed status. */
static void handle_stop(const char *id) {
    attack_stop_current();
    api_respond(id, 200, NULL, 0);
}

/* GET /capture.pcap — raw pcap buffer built during the last handshake capture */
static void handle_capture_pcap(const char *id) {
    api_respond(id, 200, pcap_serializer_get_buffer(), pcap_serializer_get_size());
}

/* GET /capture.hccapx — fixed-size hashcat hccapx struct */
static void handle_capture_hccapx(const char *id) {
    api_respond(id, 200, (const uint8_t *) hccapx_serializer_get(), sizeof(hccapx_t));
}

/* GET /evil-twin-status */
static void handle_evil_twin_status(const char *id) {
    char json[1024];
    const char *password = get_evil_twin_password();
    int wrong_attempts    = get_wrong_attempts_count();
    if (password != NULL) {
        snprintf(json, sizeof(json),
                 "{\"status\":\"SUCCESS\",\"password\":\"%s\",\"wrong_attempts\":%d}",
                 password, wrong_attempts);
    } else if (is_evil_twin_active()) {
        char wrong_pwds[512];
        get_wrong_passwords(wrong_pwds, sizeof(wrong_pwds));
        snprintf(json, sizeof(json),
                 "{\"status\":\"RUNNING\",\"wrong_attempts\":%d,\"wrong_passwords\":\"%s\"}",
                 wrong_attempts, wrong_pwds);
    } else {
        snprintf(json, sizeof(json), "{\"status\":\"STOPPED\",\"wrong_attempts\":%d}", wrong_attempts);
    }
    api_respond_text(id, 200, json);
}

/* GET /devil_twin/portal-state — is the active captive portal custom or default? */
static void handle_portal_state(const char *id) {
    uint8_t custom = 0;
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "portal_custom", &custom);
        nvs_close(nvs);
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"custom\":%s}", custom ? "true" : "false");
    api_respond_text(id, 200, json);
}

/* GET /devil_twin/index.html — active portal HTML, for the "Preview" button
 * (over WiFi this was a real link the browser navigated to directly). */
static void handle_portal_preview(const char *id) {
    FILE *f = fopen(PORTAL_ACTIVE_PATH, "r");
    if (!f) { api_respond_text(id, 404, "not found"); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); api_respond_text(id, 200, ""); return; }
    uint8_t *buf = malloc(size);
    if (!buf) { fclose(f); api_respond_text(id, 500, "oom"); return; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    api_respond(id, 200, buf, rd);
    free(buf);
}

/* GET /eviltwin-log — raw text file (uptime|ssid|bssid|username|password|status per line) */
static void handle_eviltwin_log_get(const char *id) {
    FILE *f = fopen("/spiffs/eviltwin_log.txt", "r");
    if (!f) { api_respond_text(id, 404, "not found"); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); api_respond_text(id, 200, ""); return; }
    uint8_t *buf = malloc(size);
    if (!buf) { fclose(f); api_respond_text(id, 500, "oom"); return; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    api_respond(id, 200, buf, rd);
    free(buf);
}

/* POST /eviltwin-log/clear */
static void handle_eviltwin_log_clear(const char *id) {
    remove("/spiffs/eviltwin_log.txt");
    api_respond_text(id, 200, "OK");
}

/* POST /custom-evil-twin — body: ssid=<name> (form-encoded) */
static void handle_custom_evil_twin(const char *id, const char *b64) {
    char buf[196];
    if (decode_body_text(b64, buf, sizeof(buf)) < 0) { api_respond_text(id, 400, "bad payload"); return; }
    char raw_ssid[96] = {0}, ssid[33] = {0};
    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK) {
        api_respond_text(id, 400, "Missing ssid");
        return;
    }
    url_decode(ssid, raw_ssid);
    ssid[32] = '\0';
    if (ssid[0] == '\0') { api_respond_text(id, 400, "Empty ssid"); return; }
    api_respond_text(id, 200, "OK");
    attack_method_evil_twin_custom(ssid);
}

/* POST /save_settings — body: ssid=<name>&pass=<pass>, then reboots */
static void handle_save_settings(const char *id, const char *b64) {
    char buf[256];
    if (decode_body_text(b64, buf, sizeof(buf)) < 0) { api_respond_text(id, 400, "bad payload"); return; }
    char raw_ssid[64] = {0}, raw_pass[96] = {0}, ssid[33] = {0}, pass[65] = {0};
    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK ||
        httpd_query_key_value(buf, "pass", raw_pass, sizeof(raw_pass)) != ESP_OK) {
        api_respond_text(id, 400, "Invalid data");
        return;
    }
    url_decode(ssid, raw_ssid);
    url_decode(pass, raw_pass);
    ssid[32] = '\0';
    pass[64] = '\0';

    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_str(nvs_h, "ap_ssid", ssid);
        nvs_set_str(nvs_h, "ap_pass", pass);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    api_respond_text(id, 200, "Settings Saved! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the response frame leave the UART FIFO first */
    esp_restart();
}

/* GET /get-log-url */
static void handle_get_log_url(const char *id) {
    char url[256] = "http://192.168.4.1/log";
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(url);
        nvs_get_str(nvs, "log_url", url, &len);
        nvs_close(nvs);
    }
    char response[300];
    snprintf(response, sizeof(response), "{\"url\":\"%s\"}", url);
    api_respond_text(id, 200, response);
}

/* POST /set-log-url — body: raw URL text (not form-encoded) */
static void handle_set_log_url(const char *id, const char *b64) {
    char buf[256];
    if (decode_body_text(b64, buf, sizeof(buf)) < 0) { api_respond_text(id, 400, "bad payload"); return; }
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "log_url", buf);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    api_respond_text(id, 200, "OK");
}

/* GET /detector/status */
static void handle_detector_status(const char *id) {
    const deauth_detector_status_t *s = deauth_detector_get_status();
    char json[1024] = "{\"running\":";
    strcat(json, s->running ? "true" : "false");
    strcat(json, ",\"alerts\":[");
    bool first = true;
    for (int i = 0; i < s->count; i++) {
        if (!s->entries[i].alerting) continue;
        if (!first) strcat(json, ",");
        first = false;
        char entry[128];
        snprintf(entry, sizeof(entry),
                 "{\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"count\":%d}",
                 s->entries[i].bssid[0], s->entries[i].bssid[1],
                 s->entries[i].bssid[2], s->entries[i].bssid[3],
                 s->entries[i].bssid[4], s->entries[i].bssid[5],
                 s->entries[i].count);
        strcat(json, entry);
    }
    strcat(json, "]}");
    api_respond_text(id, 200, json);
}

static void handle_detector_start(const char *id) { deauth_detector_start(); api_respond_text(id, 200, "OK"); }
static void handle_detector_stop(const char *id)  { deauth_detector_stop();  api_respond_text(id, 200, "OK"); }

/* ── Captive portal upload — chunked (files up to 100 KB don't fit on one
 * console line). The web shim splits the file into small POSTs:
 *   /devil_twin/upload/start  body: decimal total size
 *   /devil_twin/upload/chunk  body: raw chunk bytes (repeated)
 *   /devil_twin/upload/finish body: none — validates + installs atomically
 * Mirrors the size/space checks and atomic rename from the old HTTP handler. */
static FILE *s_upload_fp       = NULL;
static long  s_upload_total    = 0;
static long  s_upload_received = 0;

static void upload_abort(void) {
    if (s_upload_fp) { fclose(s_upload_fp); s_upload_fp = NULL; }
    unlink(PORTAL_TMP_PATH);
    s_upload_total = s_upload_received = 0;
}

static void handle_portal_upload_start(const char *id, const char *b64) {
    char buf[32];
    if (decode_body_text(b64, buf, sizeof(buf)) < 0) { api_respond_text(id, 400, "bad size"); return; }
    long total = atol(buf);
    if (total <= 0 || total > PORTAL_MAX_BYTES) {
        api_respond_text(id, 413, "File exceeds 100 KB limit.");
        return;
    }
    size_t fs_total = 0, fs_used = 0;
    if (esp_spiffs_info("storage", &fs_total, &fs_used) == ESP_OK) {
        size_t free_bytes = (fs_total > fs_used) ? (fs_total - fs_used) : 0;
        if (free_bytes < (size_t) total + 8192) {
            api_respond_text(id, 507, "Not enough space on device.");
            return;
        }
    }
    if (s_upload_fp) upload_abort();
    s_upload_fp = fopen(PORTAL_TMP_PATH, "w");
    if (!s_upload_fp) { api_respond_text(id, 500, "Cannot open temp file"); return; }
    s_upload_total    = total;
    s_upload_received = 0;
    api_respond_text(id, 200, "OK");
}

static void handle_portal_upload_chunk(const char *id, const char *b64) {
    if (!s_upload_fp) { api_respond_text(id, 400, "no upload in progress"); return; }
    if (!b64 || !b64[0]) { api_respond_text(id, 400, "empty chunk"); return; }
    unsigned char raw[900];
    size_t olen = 0;
    if (mbedtls_base64_decode(raw, sizeof(raw), &olen, (const unsigned char *) b64, strlen(b64)) != 0) {
        upload_abort();
        api_respond_text(id, 400, "bad chunk");
        return;
    }
    if (fwrite(raw, 1, olen, s_upload_fp) != olen) {
        upload_abort();
        api_respond_text(id, 507, "Write failed (disk full).");
        return;
    }
    s_upload_received += olen;
    api_respond_text(id, 200, "OK");
}

static void handle_portal_upload_finish(const char *id) {
    if (!s_upload_fp) { api_respond_text(id, 400, "no upload in progress"); return; }
    fclose(s_upload_fp);
    s_upload_fp = NULL;

    if (s_upload_received != s_upload_total) {
        unlink(PORTAL_TMP_PATH);
        s_upload_total = s_upload_received = 0;
        api_respond_text(id, 400, "incomplete upload");
        return;
    }
    s_upload_total = s_upload_received = 0;

    /* Same as uri_portal_upload_handler: the active file always exists, and
     * SPIFFS rename() fails if the destination exists, so remove first. */
    remove(PORTAL_ACTIVE_PATH);
    if (rename(PORTAL_TMP_PATH, PORTAL_ACTIVE_PATH) != 0) {
        unlink(PORTAL_TMP_PATH);
        api_respond_text(id, 500, "Install failed");
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "portal_custom", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    api_respond_text(id, 200, "OK");
}

/* POST /devil_twin/restore-default */
static void handle_portal_restore(const char *id) {
    FILE *src = fopen(PORTAL_DEFAULT_PATH, "r");
    if (!src) { api_respond_text(id, 500, "Default file missing"); return; }
    FILE *dst = fopen(PORTAL_ACTIVE_PATH, "w");
    if (!dst) { fclose(src); api_respond_text(id, 500, "Cannot write active file"); return; }

    char buf[1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    fclose(src);
    fclose(dst);
    if (!ok) { api_respond_text(id, 500, "Copy failed"); return; }

    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "portal_custom", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    api_respond_text(id, 200, "OK");
}

/* ── Printer tab ──────────────────────────────────────────────────────────── */

/* POST /printer/connect — body: ap=<scan index>&pass=<url-encoded, optional> */
static void handle_printer_connect(const char *id, const char *b64) {
    char buf[320];
    if (decode_body_text(b64, buf, sizeof(buf)) < 0) { api_respond_text(id, 400, "bad payload"); return; }
    char ap_str[8] = {0}, raw_pass[128] = {0}, pass[96] = {0};
    if (httpd_query_key_value(buf, "ap", ap_str, sizeof(ap_str)) != ESP_OK) {
        api_respond_text(id, 400, "Missing ap");
        return;
    }
    if (httpd_query_key_value(buf, "pass", raw_pass, sizeof(raw_pass)) == ESP_OK) {
        url_decode(pass, raw_pass);
    }
    if (printer_connect((uint8_t) atoi(ap_str), pass) != ESP_OK) {
        api_respond_text(id, 400, "Connect rejected");
        return;
    }
    api_respond_text(id, 200, "OK");
}

/* GET /printer/status */
static void handle_printer_status(const char *id) {
    char ip[16] = {0}, ssid[33] = {0};
    printer_conn_info(ip, sizeof(ip), ssid, sizeof(ssid));
    char json[160];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\"}",
             printer_conn_state_str(), ip, ssid);
    api_respond_text(id, 200, json);
}

/* POST /printer/scan */
static void handle_printer_scan(const char *id) {
    if (printer_scan_start() != ESP_OK) {
        api_respond_text(id, 400, "Not connected or busy");
        return;
    }
    api_respond_text(id, 200, "OK");
}

/* GET /printer/scan-status */
static void handle_printer_scan_status(const char *id) {
    char *json = malloc(1024);
    if (!json) { api_respond_text(id, 500, "oom"); return; }
    int off = snprintf(json, 1024, "{\"state\":\"%s\",\"progress\":%d,\"printers\":[",
                        printer_scan_state_str(), printer_scan_progress());
    int n = printer_scan_count();
    for (int i = 0; i < n && off < 1000; i++) {
        const char *ip = printer_scan_ip(i);
        if (ip == NULL) break;
        off += snprintf(json + off, 1024 - off, "%s\"%s\"", i ? "," : "", ip);
    }
    snprintf(json + off, 1024 - off, "]}");
    api_respond_text(id, 200, json);
    free(json);
}

/* POST /printer/print — body: copies=<n>&targets=<ip,ip,...>&text=<url-encoded> */
static void handle_printer_print(const char *id, const char *b64) {
    char buf[2200];
    int total = decode_body_text(b64, buf, sizeof(buf));
    if (total < 0) { api_respond_text(id, 400, "bad payload"); return; }

    char copies_str[8] = {0};
    char targets[640]  = {0};
    httpd_query_key_value(buf, "copies", copies_str, sizeof(copies_str));
    if (httpd_query_key_value(buf, "targets", targets, sizeof(targets)) != ESP_OK) {
        api_respond_text(id, 400, "No targets");
        return;
    }

    char *text = malloc((size_t) total + 1);
    if (!text) { api_respond_text(id, 500, "oom"); return; }
    text[0] = '\0';
    char *tp = strstr(buf, "text=");   /* text is the last field — never contains a raw '&' */
    if (tp != NULL) url_decode(text, tp + 5);

    esp_err_t pr = printer_print_start(targets, atoi(copies_str), text);
    free(text);   /* printer_print_start copies it internally */
    if (pr != ESP_OK) {
        api_respond_text(id, 400, "Print rejected");
        return;
    }
    api_respond_text(id, 200, "OK");
}

/* GET /printer/job-status */
static void handle_printer_job(const char *id) {
    char json[128];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"done\":%d,\"ok\":%d,\"total\":%d}",
             printer_job_state_str(), printer_job_done(), printer_job_ok(), printer_job_total());
    api_respond_text(id, 200, json);
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */
static int cmd_api(int argc, char **argv) {
    if (argc < 4) { printf("@RES ? 400 0 \n"); fflush(stdout); return 0; }
    const char *id   = argv[1];
    const char *verb = argv[2];
    const char *path = argv[3];
    const char *body = (argc >= 5) ? argv[4] : NULL;
    bool is_get  = !strcmp(verb, "GET");
    bool is_post = !strcmp(verb, "POST");

    if      (is_get  && !strcmp(path, "/ap-list"))                      handle_ap_list(id);
    else if (is_get  && !strcmp(path, "/status"))                       handle_status(id);
    else if (is_post && !strcmp(path, "/run-attack"))                   handle_run_attack(id, body);
    else if (!strcmp(path, "/reset"))                                   handle_reset(id);
    else if (!strcmp(path, "/stop"))                                    handle_stop(id);
    else if (!strcmp(path, "/ping"))                                    api_respond_text(id, 200, "pong");

    else if (is_get  && !strcmp(path, "/capture.pcap"))                 handle_capture_pcap(id);
    else if (is_get  && !strcmp(path, "/capture.hccapx"))                handle_capture_hccapx(id);

    else if (is_get  && !strcmp(path, "/evil-twin-status"))              handle_evil_twin_status(id);
    else if (is_get  && !strcmp(path, "/devil_twin/portal-state"))       handle_portal_state(id);
    else if (is_get  && !strcmp(path, "/devil_twin/index.html"))         handle_portal_preview(id);
    else if (is_get  && !strcmp(path, "/eviltwin-log"))                  handle_eviltwin_log_get(id);
    else if (is_post && !strcmp(path, "/eviltwin-log/clear"))            handle_eviltwin_log_clear(id);
    else if (is_post && !strcmp(path, "/custom-evil-twin"))              handle_custom_evil_twin(id, body);
    else if (is_post && !strcmp(path, "/devil_twin/upload/start"))       handle_portal_upload_start(id, body);
    else if (is_post && !strcmp(path, "/devil_twin/upload/chunk"))       handle_portal_upload_chunk(id, body);
    else if (is_post && !strcmp(path, "/devil_twin/upload/finish"))      handle_portal_upload_finish(id);
    else if (is_post && !strcmp(path, "/devil_twin/restore-default"))    handle_portal_restore(id);

    else if (is_post && !strcmp(path, "/save_settings"))                 handle_save_settings(id, body);
    else if (is_get  && !strcmp(path, "/get-log-url"))                   handle_get_log_url(id);
    else if (is_post && !strcmp(path, "/set-log-url"))                   handle_set_log_url(id, body);

    else if (is_get  && !strcmp(path, "/detector/status"))               handle_detector_status(id);
    else if (is_post && !strcmp(path, "/detector/start"))                handle_detector_start(id);
    else if (is_post && !strcmp(path, "/detector/stop"))                 handle_detector_stop(id);

    else if (is_post && !strcmp(path, "/printer/connect"))               handle_printer_connect(id, body);
    else if (is_get  && !strcmp(path, "/printer/status"))                handle_printer_status(id);
    else if (is_post && !strcmp(path, "/printer/scan"))                  handle_printer_scan(id);
    else if (is_get  && !strcmp(path, "/printer/scan-status"))           handle_printer_scan_status(id);
    else if (is_post && !strcmp(path, "/printer/print"))                 handle_printer_print(id, body);
    else if (is_get  && !strcmp(path, "/printer/job-status"))            handle_printer_job(id);

    else                                                                 api_respond_text(id, 404, "unknown endpoint");
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
