/**
 * @file serial_api.h
 * @brief Machine-readable "API mode" bridge over the serial console.
 *
 * Lets the hosted web UI (Web Serial / WebUSB) drive the device using the SAME
 * binary payloads the old WiFi HTTP endpoints produced, transported as framed,
 * base64-encoded lines. Registered as the `api` console command so it coexists
 * with the human REPL.
 *
 * Wire protocol (one line each):
 *   request :  api <id> <VERB> <path> [<base64-body>]
 *   response:  @RES <id> <status> <len> <base64-payload>
 * where <len> is the number of RAW bytes encoded in the base64 payload.
 */
#ifndef SERIAL_API_H
#define SERIAL_API_H

/** Registers the `api` command on the esp_console REPL. */
void serial_api_register(void);

#endif
