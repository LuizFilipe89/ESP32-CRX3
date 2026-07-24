/**
 * @file printer.h
 * @brief Network printer module: joins a chosen Wi-Fi network as a station,
 *        scans the local subnet for printers on port 9100 (raw/JetDirect,
 *        mainly HP-style), 631 (IPP), or 515 (LPR), and sends a print job to
 *        selected printers — raw PJL/PCL where 9100 is open, falling back to
 *        an IPP Print-Job request (what AirPrint-class printers, including
 *        most consumer Epson inkjets, actually understand) otherwise.
 */
#ifndef PRINTER_H
#define PRINTER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ── Connection ──────────────────────────────────────────────────────────── */

/**
 * @brief Connects the STA interface to a scanned AP (by its scan-list index).
 *        The management AP keeps running (APSTA), so the web UI stays reachable.
 *
 * @param ap_index index into the wifi_controller scan list (same ids as /ap-list)
 * @param password network password; pass "" (empty) for an open network
 */
esp_err_t printer_connect(uint8_t ap_index, const char *password);

/** @return "idle" | "connecting" | "connected" | "failed" */
const char *printer_conn_state_str(void);

/** Fills the assigned STA IP and the joined SSID (empty strings if none). */
void printer_conn_info(char *ip_out, size_t ip_len, char *ssid_out, size_t ssid_len);

/* ── Subnet scan ─────────────────────────────────────────────────────────── */

/** Starts an asynchronous scan of the local /24 subnet for printers on ports 9100, 631, or 515. */
esp_err_t printer_scan_start(void);

/** @return "idle" | "scanning" | "done" */
const char *printer_scan_state_str(void);

/** @return scan completion percentage 0..100 */
int printer_scan_progress(void);

/** @return number of printers found so far */
int printer_scan_count(void);

/** @return pointer to the i-th found printer IP string, or NULL if out of range */
const char *printer_scan_ip(int index);

/* ── Print ───────────────────────────────────────────────────────────────── */

/**
 * @brief Starts an asynchronous print job to one or more printers.
 *
 * @param targets_csv comma-separated printer IPs, e.g. "192.168.1.5,192.168.1.9"
 * @param copies      number of copies (clamped to 1..99) via @PJL SET COPIES
 * @param text        the plain text to print (already URL-decoded)
 */
esp_err_t printer_print_start(const char *targets_csv, int copies, const char *text);

/** @return "idle" | "printing" | "done" */
const char *printer_job_state_str(void);

int printer_job_total(void);   /**< number of target printers in the current job */
int printer_job_done(void);    /**< printers processed so far */
int printer_job_ok(void);      /**< printers the job was delivered to successfully */

#endif /* PRINTER_H */
