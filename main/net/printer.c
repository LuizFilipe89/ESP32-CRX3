/**
 * @file printer.c
 * @brief Implementation of the network-printer module (see printer.h).
 */
#include "printer.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "wifi_controller.h"

static const char *TAG = "printer";

#define PRN_PORT           9100
#define PRN_MAX_PRINTERS   32
#define PRN_SCAN_BATCH     8
#define PRN_SCAN_TMO_US    400000     /* 400 ms per connect batch */
#define PRN_SCAN_MAX_HOSTS 512        /* clamp very large subnets */
#define PRN_CONNECT_TMO_MS 4000

/* ── Connection state ────────────────────────────────────────────────────── */
typedef enum { CONN_IDLE, CONN_CONNECTING, CONN_CONNECTED, CONN_FAILED } conn_state_t;
static volatile conn_state_t conn_state = CONN_IDLE;
static esp_netif_ip_info_t   sta_ip;
static char                  conn_ssid[33] = {0};

static uint8_t pending_ap_index = 0;
static char    pending_pass[64] = {0};

/* ── Scan state ──────────────────────────────────────────────────────────── */
typedef enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE } scan_state_t;
static volatile scan_state_t scan_state    = SCAN_IDLE;
static volatile int          scan_progress = 0;
static char                  found_ips[PRN_MAX_PRINTERS][16];
static volatile int          found_count   = 0;

/* ── Print job state ─────────────────────────────────────────────────────── */
typedef enum { JOB_IDLE, JOB_RUNNING, JOB_DONE } job_state_t;
static volatile job_state_t job_state = JOB_IDLE;
static volatile int         job_total = 0, job_done = 0, job_ok = 0;
static int                  job_copies = 1;
static char                 job_targets[PRN_MAX_PRINTERS * 16 + 16] = {0};
static char                *job_text = NULL;

/* Universal Exit Language — brackets a PJL job. */
static const char PJL_UEL[] = "\x1B%-12345X";

/* ───────────────────────────── Connection ──────────────────────────────── */

static void connect_task(void *arg) {
    const wifi_ap_record_t *rec = wifictl_get_ap_record(pending_ap_index);
    if (rec == NULL) {
        conn_state = CONN_FAILED;
        vTaskDelete(NULL);
        return;
    }

    /* Drop any previous STA association before joining the new network. */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifictl_sta_connect_to_ap(rec, pending_pass);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 200 && conn_state == CONN_CONNECTING; i++) {   /* up to 20 s */
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_netif_ip_info_t ip;
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            sta_ip = ip;
            conn_state = CONN_CONNECTED;
            ESP_LOGI(TAG, "Joined '%s', IP " IPSTR, conn_ssid, IP2STR(&ip.ip));
            break;
        }
    }
    if (conn_state != CONN_CONNECTED) {
        conn_state = CONN_FAILED;
        ESP_LOGW(TAG, "Failed to join '%s' (wrong password or no DHCP)", conn_ssid);
    }
    vTaskDelete(NULL);
}

esp_err_t printer_connect(uint8_t ap_index, const char *password) {
    if (conn_state == CONN_CONNECTING) return ESP_ERR_INVALID_STATE;

    const wifi_ap_record_t *rec = wifictl_get_ap_record(ap_index);
    if (rec == NULL) return ESP_ERR_NOT_FOUND;

    strncpy(conn_ssid, (char *) rec->ssid, sizeof(conn_ssid) - 1);
    conn_ssid[sizeof(conn_ssid) - 1] = '\0';
    pending_ap_index = ap_index;
    strncpy(pending_pass, password ? password : "", sizeof(pending_pass) - 1);
    pending_pass[sizeof(pending_pass) - 1] = '\0';

    /* leaving a previous session? reset scan results */
    scan_state = SCAN_IDLE;
    found_count = 0;
    scan_progress = 0;

    conn_state = CONN_CONNECTING;
    xTaskCreate(connect_task, "prn_conn", 4096, NULL, 5, NULL);
    return ESP_OK;
}

const char *printer_conn_state_str(void) {
    switch (conn_state) {
        case CONN_CONNECTING: return "connecting";
        case CONN_CONNECTED:  return "connected";
        case CONN_FAILED:     return "failed";
        default:              return "idle";
    }
}

void printer_conn_info(char *ip_out, size_t ip_len, char *ssid_out, size_t ssid_len) {
    if (ip_out && ip_len) {
        if (conn_state == CONN_CONNECTED) {
            snprintf(ip_out, ip_len, IPSTR, IP2STR(&sta_ip.ip));
        } else {
            ip_out[0] = '\0';
        }
    }
    if (ssid_out && ssid_len) {
        strncpy(ssid_out, conn_ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
    }
}

/* ───────────────────────────── Subnet scan ─────────────────────────────── */

static void scan_task(void *arg) {
    scan_state    = SCAN_RUNNING;
    found_count   = 0;
    scan_progress = 0;

    uint32_t ip   = ntohl(sta_ip.ip.addr);
    uint32_t mask = ntohl(sta_ip.netmask.addr);
    uint32_t net  = ip & mask;
    uint32_t bcast = net | (~mask);
    uint32_t first = net + 1;
    uint32_t last  = (bcast > 0) ? bcast - 1 : first;
    if (last < first) last = first;
    if (last - first + 1 > PRN_SCAN_MAX_HOSTS) last = first + PRN_SCAN_MAX_HOSTS - 1;

    uint32_t total = last - first + 1;
    uint32_t scanned = 0;

    for (uint32_t h = first; h <= last && found_count < PRN_MAX_PRINTERS; ) {
        int fds[PRN_SCAN_BATCH];
        uint32_t addrs[PRN_SCAN_BATCH];
        int nb = 0;

        for (; nb < PRN_SCAN_BATCH && h <= last; h++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) continue;
            int fl = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, fl | O_NONBLOCK);

            struct sockaddr_in sa = {0};
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(PRN_PORT);
            sa.sin_addr.s_addr = htonl(h);

            int r = connect(s, (struct sockaddr *) &sa, sizeof(sa));
            if (r == 0 || (r < 0 && errno == EINPROGRESS)) {
                addrs[nb] = h;
                fds[nb]   = s;
                nb++;
            } else {
                close(s);
            }
        }

        if (nb > 0) {
            fd_set wf;
            FD_ZERO(&wf);
            int maxfd = -1;
            for (int i = 0; i < nb; i++) {
                FD_SET(fds[i], &wf);
                if (fds[i] > maxfd) maxfd = fds[i];
            }
            struct timeval tv = { .tv_sec = 0, .tv_usec = PRN_SCAN_TMO_US };
            select(maxfd + 1, NULL, &wf, NULL, &tv);

            for (int i = 0; i < nb; i++) {
                if (FD_ISSET(fds[i], &wf)) {
                    int err = 0;
                    socklen_t l = sizeof(err);
                    getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &l);
                    if (err == 0 && found_count < PRN_MAX_PRINTERS) {
                        struct in_addr ina = { .s_addr = htonl(addrs[i]) };
                        strncpy(found_ips[found_count], inet_ntoa(ina), 15);
                        found_ips[found_count][15] = '\0';
                        ESP_LOGI(TAG, "Printer found: %s", found_ips[found_count]);
                        found_count++;
                    }
                }
                close(fds[i]);
            }
        }

        scanned += nb;
        scan_progress = total ? (int)((scanned * 100) / total) : 100;
        vTaskDelay(1);   /* yield to keep the web server responsive */
    }

    scan_progress = 100;
    scan_state    = SCAN_DONE;
    ESP_LOGI(TAG, "Scan done: %d printer(s) found", found_count);
    vTaskDelete(NULL);
}

esp_err_t printer_scan_start(void) {
    if (conn_state != CONN_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (scan_state == SCAN_RUNNING)   return ESP_ERR_INVALID_STATE;
    xTaskCreate(scan_task, "prn_scan", 4096, NULL, 5, NULL);
    return ESP_OK;
}

const char *printer_scan_state_str(void) {
    switch (scan_state) {
        case SCAN_RUNNING: return "scanning";
        case SCAN_DONE:    return "done";
        default:           return "idle";
    }
}

int printer_scan_progress(void) { return scan_progress; }
int printer_scan_count(void)    { return found_count; }

const char *printer_scan_ip(int index) {
    if (index < 0 || index >= found_count) return NULL;
    return found_ips[index];
}

/* ─────────────────────────────── Print ─────────────────────────────────── */

/* Blocking connect with a timeout (non-blocking connect + select). */
static int connect_timeout(const char *ip, int port, int tmo_ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);

    int r = connect(s, (struct sockaddr *) &sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) { close(s); return -1; }

    if (r != 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv = { .tv_sec = tmo_ms / 1000, .tv_usec = (tmo_ms % 1000) * 1000 };
        if (select(s + 1, NULL, &wf, NULL, &tv) <= 0) { close(s); return -1; }
        int err = 0;
        socklen_t l = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err != 0) { close(s); return -1; }
    }

    fcntl(s, F_SETFL, fl);   /* back to blocking for send() */
    return s;
}

static bool send_all(int s, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static void print_task(void *arg) {
    job_state = JOB_RUNNING;
    job_done  = 0;
    job_ok    = 0;

    char header[96];
    int hlen = snprintf(header, sizeof(header),
                        "@PJL\r\n@PJL SET COPIES=%d\r\n@PJL ENTER LANGUAGE=PCL\r\n",
                        job_copies);
    const char trailer[] = "\r\n\x0C";   /* CRLF + form feed (eject page) */

    char *saveptr = NULL;
    char *tok = strtok_r(job_targets, ",", &saveptr);
    while (tok != NULL) {
        while (*tok == ' ') tok++;
        bool ok = false;
        int s = connect_timeout(tok, PRN_PORT, PRN_CONNECT_TMO_MS);
        if (s >= 0) {
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ok  = send_all(s, PJL_UEL, strlen(PJL_UEL));
            ok  = ok && send_all(s, header, hlen);
            ok  = ok && (job_text == NULL || send_all(s, job_text, strlen(job_text)));
            ok  = ok && send_all(s, trailer, strlen(trailer));
            ok  = ok && send_all(s, PJL_UEL, strlen(PJL_UEL));
            close(s);
            ESP_LOGI(TAG, "Print -> %s : %s", tok, ok ? "sent" : "send error");
        } else {
            ESP_LOGW(TAG, "Print -> %s : connect failed", tok);
        }
        if (ok) job_ok++;
        job_done++;
        tok = strtok_r(NULL, ",", &saveptr);
    }

    if (job_text) { free(job_text); job_text = NULL; }
    job_state = JOB_DONE;
    vTaskDelete(NULL);
}

esp_err_t printer_print_start(const char *targets_csv, int copies, const char *text) {
    if (conn_state != CONN_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (job_state == JOB_RUNNING)     return ESP_ERR_INVALID_STATE;
    if (targets_csv == NULL || targets_csv[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (copies < 1)  copies = 1;
    if (copies > 99) copies = 99;
    job_copies = copies;

    strncpy(job_targets, targets_csv, sizeof(job_targets) - 1);
    job_targets[sizeof(job_targets) - 1] = '\0';

    /* count targets */
    job_total = 1;
    for (const char *p = job_targets; *p; p++) if (*p == ',') job_total++;

    if (job_text) free(job_text);
    job_text = strdup(text ? text : "");
    if (job_text == NULL) return ESP_ERR_NO_MEM;

    job_done  = 0;
    job_ok    = 0;
    job_state = JOB_RUNNING;
    xTaskCreate(print_task, "prn_print", 6144, NULL, 5, NULL);
    return ESP_OK;
}

const char *printer_job_state_str(void) {
    switch (job_state) {
        case JOB_RUNNING: return "printing";
        case JOB_DONE:    return "done";
        default:          return "idle";
    }
}

int printer_job_total(void) { return job_total; }
int printer_job_done(void)  { return job_done; }
int printer_job_ok(void)    { return job_ok; }
