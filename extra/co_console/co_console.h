#pragma once
#include "esp_err.h"

// Sets up the node's diagnostic console. Call first thing in app_main, before
// anything else logs and before registering any esp_console commands:
//  - captures stdout into the log ring buffer (log_ringbuffer.h),
//  - mirrors it to the chip's console port (USB-Serial-JTAG or UART, per
//    CONFIG_ESP_CONSOLE_*) when CONFIG_CO_CONSOLE_SERIAL_OUTPUT is set,
//  - initializes esp_console and registers "help" and "logbuf",
//  - wires OD 0x2050 (logbuf, SDO upload of the ring) and OD 0x2051
//    (command, SDO-written command line run through esp_console),
//  - with CONFIG_CO_CONSOLE_REPL, starts an interactive REPL on the console
//    port. On USB-Serial-JTAG it stays idle until a host is connected.
// `prompt` must stay valid for the program lifetime (a string literal).
void co_console_init(const char *prompt);

// Runs one command line through esp_console, serialized against the other
// command sources (REPL, OD 0x2051, ota_logs TCP console), and logs the
// outcome if it failed. `source` names the caller in that log line.
esp_err_t co_console_run(const char *source, const char *line);
