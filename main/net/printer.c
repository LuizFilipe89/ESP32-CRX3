/**
 * @file printer.c
 * @brief Implementation of the network-printer module (see printer.h).
 */
#include "printer.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
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

/* HP-style printers (JetDirect/LaserJet) reliably speak raw PCL/PJL on
 * port 9100. Cheap Epson inkjets (L-series and similar) very often don't
 * open 9100 at all — they only understand their own ESC/P-R raster format,
 * and many units ship with raw/JetDirect printing disabled by default. Per
 * Epson's own published port list, 631 (IPP — what AirPrint/"IPP Everywhere"
 * uses) and 515 (LPR) are supported across their printer line too, so a scan
 * that only checks 9100 misses most consumer Epson units entirely. Checking
 * all three, and falling back to an IPP print job when 9100 isn't there,
 * covers both worlds instead of assuming every printer is HP-shaped. */
#define PRN_PORT_RAW       9100
#define PRN_PORT_IPP       631
#define PRN_PORT_LPR       515
static const uint16_t PRN_SCAN_PORTS[] = { PRN_PORT_RAW, PRN_PORT_IPP, PRN_PORT_LPR };
#define PRN_SCAN_PORT_COUNT (sizeof(PRN_SCAN_PORTS) / sizeof(PRN_SCAN_PORTS[0]))

#define PRN_MAX_PRINTERS   32
#define PRN_SCAN_BATCH     8
/* 600ms, not the original 400ms — printers in a power-save/sleep state can
 * take noticeably longer than an always-on device to wake their network
 * stack and complete a TCP handshake, and a too-short window here reads as
 * "nothing found" for a printer that's simply asleep. */
#define PRN_SCAN_TMO_US    600000
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

static bool host_already_found(uint32_t h) {
    struct in_addr ina = { .s_addr = htonl(h) };
    const char *s = inet_ntoa(ina);
    for (int i = 0; i < found_count; i++) {
        if (strcmp(found_ips[i], s) == 0) return true;
    }
    return false;
}

/* One full sweep of [first, last] against a single port. Hosts already in
 * found_ips (from an earlier port's pass) are skipped so a printer that
 * answers on more than one port is only reported once. progress_base/
 * progress_share let the three passes in scan_task() each own a slice of
 * the overall 0..100 progress bar instead of each one visibly resetting it. */
static void scan_port_pass(uint32_t first, uint32_t last, uint16_t port,
                            int progress_base, int progress_share) {
    uint32_t total = last - first + 1;
    uint32_t scanned = 0;

    for (uint32_t h = first; h <= last && found_count < PRN_MAX_PRINTERS; ) {
        int fds[PRN_SCAN_BATCH];
        uint32_t addrs[PRN_SCAN_BATCH];
        int nb = 0;

        for (; nb < PRN_SCAN_BATCH && h <= last; h++) {
            if (host_already_found(h)) { scanned++; continue; }

            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) continue;
            int fl = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, fl | O_NONBLOCK);

            struct sockaddr_in sa = {0};
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(port);
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
                        ESP_LOGI(TAG, "Printer found: %s (port %u)", found_ips[found_count], port);
                        found_count++;
                    }
                }
                close(fds[i]);
            }
            scanned += nb;
        }

        int pass_pct = total ? (int)((scanned * 100) / total) : 100;
        scan_progress = progress_base + (pass_pct * progress_share) / 100;
        vTaskDelay(1);   /* yield to keep the web server responsive */
    }
}

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

    /* Raw (9100) first — it's the cheapest to act on later — then IPP (631),
     * then LPR (515), each pass sharing an equal slice of the progress bar. */
    int share = 100 / PRN_SCAN_PORT_COUNT;
    for (size_t p = 0; p < PRN_SCAN_PORT_COUNT && found_count < PRN_MAX_PRINTERS; p++) {
        scan_port_pass(first, last, PRN_SCAN_PORTS[p], (int)(p * share), share);
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

/* Appends one IPP attribute (tag + name + value, each length-prefixed per
 * RFC 8010 §3.5) to buf, bounds-checked against buf_size. */
static bool ipp_put_attr(uint8_t *buf, size_t buf_size, size_t *p,
                          uint8_t tag, const char *name, const char *value) {
    size_t nl = strlen(name), vl = strlen(value);
    if (*p + 1 + 2 + nl + 2 + vl > buf_size) return false;
    buf[(*p)++] = tag;
    buf[(*p)++] = (nl >> 8) & 0xFF;
    buf[(*p)++] = nl & 0xFF;
    memcpy(&buf[*p], name, nl); *p += nl;
    buf[(*p)++] = (vl >> 8) & 0xFF;
    buf[(*p)++] = vl & 0xFF;
    memcpy(&buf[*p], value, vl); *p += vl;
    return true;
}

/* Same as ipp_put_attr() but for a 4-byte signed integer value (tag 0x21),
 * e.g. "copies" — a real Job Template attribute, unlike the operation
 * attributes above. */
static bool ipp_put_attr_int(uint8_t *buf, size_t buf_size, size_t *p,
                              uint8_t tag, const char *name, int32_t value) {
    size_t nl = strlen(name);
    if (*p + 1 + 2 + nl + 2 + 4 > buf_size) return false;
    buf[(*p)++] = tag;
    buf[(*p)++] = (nl >> 8) & 0xFF;
    buf[(*p)++] = nl & 0xFF;
    memcpy(&buf[*p], name, nl); *p += nl;
    buf[(*p)++] = 0x00; buf[(*p)++] = 0x04;
    buf[(*p)++] = (value >> 24) & 0xFF;
    buf[(*p)++] = (value >> 16) & 0xFF;
    buf[(*p)++] = (value >> 8) & 0xFF;
    buf[(*p)++] = value & 0xFF;
    return true;
}

/* ── Minimal PDF builder ─────────────────────────────────────────────────── *
 * Per the Printer Working Group's own IPP client guide, "application/
 * octet-stream" (auto-detect document-format) is always accepted per spec
 * but "detection accuracy varies widely" in practice, and plain "text/plain"
 * isn't guaranteed to be accepted by an IPP printer at all — "application/
 * pdf" is what's actually most broadly supported. Handwritten instead of
 * pulling in a PDF library because the document is trivial: one font,
 * left-aligned lines, no images — well within reach of PDF's plain-text
 * object syntax (a valid PDF is mostly just readable ASCII + a byte-offset
 * index at the end). */
#define PDF_CHARS_PER_LINE 80
#define PDF_LINES_PER_PAGE 45
#define PDF_MAX_PAGES      2
#define PDF_MAX_TEXT_CHARS (PDF_CHARS_PER_LINE * PDF_LINES_PER_PAGE * PDF_MAX_PAGES)
#define PDF_BUF_CAP        24576
#define PDF_CONTENT_CAP    10240

static bool pdf_appendf(uint8_t *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *) buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= cap - *pos) return false;
    *pos += (size_t) n;
    return true;
}

/* Writes one line as a PDF string-literal "Tj" show-text op, escaping the
 * three bytes a "(...)" literal requires escaped. Non-Latin-1 bytes (e.g.
 * multi-byte UTF-8 accented characters) pass through as-is and will render
 * as garbage under base-14 Helvetica/WinAnsiEncoding — acceptable for a
 * nuisance/spam payload, not a document printer. */
static bool pdf_append_escaped_line(uint8_t *buf, size_t cap, size_t *pos,
                                     const char *line, size_t line_len) {
    char esc[PDF_CHARS_PER_LINE * 2 + 1];
    size_t e = 0;
    for (size_t i = 0; i < line_len && e < sizeof(esc) - 1; i++) {
        char c = line[i];
        if (c == '\\' || c == '(' || c == ')') esc[e++] = '\\';
        esc[e++] = c;
    }
    esc[e] = '\0';
    return pdf_appendf(buf, cap, pos, "(%s) Tj\nT*\n", esc);
}

/* Builds a minimal multi-page PDF wrapping `text` as hard-wrapped Helvetica
 * body lines (up to PDF_MAX_PAGES pages; anything past PDF_MAX_TEXT_CHARS is
 * silently dropped — this is a nuisance payload, not a full document
 * formatter). Returns a malloc'd buffer (caller frees) and its length via
 * *out_len, or NULL on failure. */
static uint8_t *build_pdf(const char *text, size_t *out_len) {
    size_t text_len = text ? strlen(text) : 0;
    if (text_len > PDF_MAX_TEXT_CHARS) text_len = PDF_MAX_TEXT_CHARS;

    size_t total_lines = (text_len + PDF_CHARS_PER_LINE - 1) / PDF_CHARS_PER_LINE;
    if (total_lines == 0) total_lines = 1;
    size_t total_pages = (total_lines + PDF_LINES_PER_PAGE - 1) / PDF_LINES_PER_PAGE;
    if (total_pages == 0) total_pages = 1;
    if (total_pages > PDF_MAX_PAGES) total_pages = PDF_MAX_PAGES;

    uint8_t *buf = malloc(PDF_BUF_CAP);
    if (!buf) return NULL;
    uint8_t *content = malloc(PDF_CONTENT_CAP);
    if (!content) { free(buf); return NULL; }

    size_t pos = 0;
    size_t total_objs = 3 + total_pages * 2;
    size_t offsets[3 + PDF_MAX_PAGES * 2 + 1] = {0};   /* 1-based, by object number */

    bool ok = pdf_appendf(buf, PDF_BUF_CAP, &pos, "%%PDF-1.4\n");

    offsets[1] = pos;
    ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    offsets[2] = pos;
    ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos, "2 0 obj\n<< /Type /Pages /Kids [");
    for (size_t i = 0; i < total_pages && ok; i++) {
        ok = pdf_appendf(buf, PDF_BUF_CAP, &pos, "%u 0 R ", (unsigned)(4 + i * 2));
    }
    ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos, "] /Count %u >>\nendobj\n", (unsigned) total_pages);

    offsets[3] = pos;
    ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos,
        "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");

    const char *cursor = text;
    size_t remaining = text_len;

    for (size_t pg = 0; pg < total_pages && ok; pg++) {
        size_t page_obj = 4 + pg * 2, content_obj = 5 + pg * 2;

        /* Content stream is built separately first because PDF requires its
         * exact byte length up front (/Length), before the stream body. */
        size_t clen = 0;
        ok = pdf_appendf(content, PDF_CONTENT_CAP, &clen, "BT /F1 11 Tf 40 750 Td 13 TL\n");
        for (size_t ln = 0; ln < PDF_LINES_PER_PAGE && remaining > 0 && ok; ln++) {
            size_t take = remaining < PDF_CHARS_PER_LINE ? remaining : PDF_CHARS_PER_LINE;
            ok = pdf_append_escaped_line(content, PDF_CONTENT_CAP, &clen, cursor, take);
            cursor += take;
            remaining -= take;
        }
        ok = ok && pdf_appendf(content, PDF_CONTENT_CAP, &clen, "ET\n");

        offsets[page_obj] = pos;
        ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos,
            "%u 0 obj\n<< /Type /Page /Parent 2 0 R "
            "/Resources << /Font << /F1 3 0 R >> >> "
            "/MediaBox [0 0 612 792] /Contents %u 0 R >>\nendobj\n",
            (unsigned) page_obj, (unsigned) content_obj);

        offsets[content_obj] = pos;
        ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos, "%u 0 obj\n<< /Length %u >>\nstream\n",
                                (unsigned) content_obj, (unsigned) clen);
        if (ok && pos + clen <= PDF_BUF_CAP) {
            memcpy(buf + pos, content, clen);
            pos += clen;
        } else {
            ok = false;
        }
        ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos, "\nendstream\nendobj\n");
    }

    free(content);
    if (!ok) { free(buf); return NULL; }

    size_t xref_offset = pos;
    ok = pdf_appendf(buf, PDF_BUF_CAP, &pos, "xref\n0 %u\n0000000000 65535 f \n", (unsigned)(total_objs + 1));
    for (size_t i = 1; i <= total_objs && ok; i++) {
        ok = pdf_appendf(buf, PDF_BUF_CAP, &pos, "%010u 00000 n \n", (unsigned) offsets[i]);
    }
    ok = ok && pdf_appendf(buf, PDF_BUF_CAP, &pos,
        "trailer\n<< /Size %u /Root 1 0 R >>\nstartxref\n%u\n%%%%EOF\n",
        (unsigned)(total_objs + 1), (unsigned) xref_offset);

    if (!ok) { free(buf); return NULL; }

    *out_len = pos;
    return buf;
}

/* Minimal IPP client — just enough of the Print-Job operation (RFC 8010/
 * 2911) to get a job accepted by "IPP Everywhere"/AirPrint-class printers,
 * which by now is effectively every modern inkjet (Epson included — it's
 * what lets them print driver-free from a phone) even when they don't have
 * a raw JetDirect port open at all. The response isn't parsed in any
 * detail — any reply at all after sending the job means the printer's IPP
 * server accepted the request instead of just resetting the connection,
 * which is as much as the ESP32 side can honestly claim to know. */
static bool send_ipp_print(const char *ip, const char *text, int copies) {
    size_t pdf_len = 0;
    uint8_t *pdf = build_pdf(text, &pdf_len);
    if (!pdf) return false;

    int s = connect_timeout(ip, PRN_PORT_IPP, PRN_CONNECT_TMO_MS);
    if (s < 0) { free(pdf); return false; }

    uint8_t ipp[400];
    size_t p = 0;
    ipp[p++] = 0x01; ipp[p++] = 0x01;                                      // IPP/1.1
    ipp[p++] = 0x00; ipp[p++] = 0x02;                                      // operation-id: Print-Job
    ipp[p++] = 0x00; ipp[p++] = 0x00; ipp[p++] = 0x00; ipp[p++] = 0x01;    // request-id = 1
    ipp[p++] = 0x01;                                                        // operation-attributes-tag

    char printer_uri[48];
    snprintf(printer_uri, sizeof(printer_uri), "ipp://%s/ipp/print", ip);

    bool ok =
        ipp_put_attr(ipp, sizeof(ipp), &p, 0x47, "attributes-charset", "utf-8") &&
        ipp_put_attr(ipp, sizeof(ipp), &p, 0x48, "attributes-natural-language", "en") &&
        ipp_put_attr(ipp, sizeof(ipp), &p, 0x45, "printer-uri", printer_uri) &&
        ipp_put_attr(ipp, sizeof(ipp), &p, 0x42, "requesting-user-name", "crx3") &&
        ipp_put_attr(ipp, sizeof(ipp), &p, 0x42, "job-name", "crx3-print") &&
        ipp_put_attr(ipp, sizeof(ipp), &p, 0x49, "document-format", "application/pdf");
    if (ok && copies > 1) {
        ok = (p + 1 <= sizeof(ipp));
        if (ok) ipp[p++] = 0x02;   // job-attributes-tag — copies is a real Job Template attribute
        ok = ok && ipp_put_attr_int(ipp, sizeof(ipp), &p, 0x21, "copies", copies);
    }
    ok = ok && (p + 1 <= sizeof(ipp));
    if (!ok) { close(s); free(pdf); return false; }
    ipp[p++] = 0x03;   // end-of-attributes-tag

    char http_header[160];
    int hlen = snprintf(http_header, sizeof(http_header),
        "POST /ipp/print HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        ip, PRN_PORT_IPP, (unsigned)(p + pdf_len));

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ok = hlen > 0 && (size_t) hlen < sizeof(http_header)
      && send_all(s, http_header, hlen)
      && send_all(s, (char *) ipp, p)
      && send_all(s, (char *) pdf, pdf_len);

    if (ok) {
        char resp[32];
        struct timeval rtv = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        ok = recv(s, resp, sizeof(resp), 0) > 0;
    }

    close(s);
    free(pdf);
    return ok;
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
        const char *method = "raw";

        int s = connect_timeout(tok, PRN_PORT_RAW, PRN_CONNECT_TMO_MS);
        if (s >= 0) {
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ok  = send_all(s, PJL_UEL, strlen(PJL_UEL));
            ok  = ok && send_all(s, header, hlen);
            ok  = ok && (job_text == NULL || send_all(s, job_text, strlen(job_text)));
            ok  = ok && send_all(s, trailer, strlen(trailer));
            ok  = ok && send_all(s, PJL_UEL, strlen(PJL_UEL));
            close(s);
        } else {
            /* No raw JetDirect port — most Epson consumer inkjets don't open
             * one at all; they don't speak PCL/PJL like HP's LaserJets do.
             * Fall back to IPP (631) so the job still has a real path in. */
            method = "ipp";
            ok = send_ipp_print(tok, job_text, job_copies);
        }

        ESP_LOGI(TAG, "Print -> %s via %s: %s", tok, method, ok ? "sent" : "failed");
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
