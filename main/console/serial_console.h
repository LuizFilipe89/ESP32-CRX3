/**
 * @file serial_console.h
 * @brief USB/UART interactive console for controlling crx3 from a serial
 *        terminal (e.g. a phone "Serial USB Terminal" app), as an alternative
 *        to the web interface.
 */
#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

/**
 * @brief Starts the interactive serial console (REPL) on the default UART.
 *
 * Registers all commands and starts the REPL task. Must be called after
 * attack_init() and after the Wi-Fi radio has been initialised.
 */
void serial_console_start(void);

#endif
