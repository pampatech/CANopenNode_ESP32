#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "sdkconfig.h"

// Number of most-recent log lines retained for later diagnosis (e.g. via the
// "logbuf" console command or a CANopen SDO domain read of OD 0x2050).
#define LOG_RINGBUFFER_LINES CONFIG_CO_CONSOLE_LOG_LINES
#define LOG_RINGBUFFER_LINE_LEN CONFIG_CO_CONSOLE_LOG_LINE_LEN

typedef int (*raw_writer_t)(const char *data, int size);

// Installs the ring buffer as stdout itself (via fwopen), so every
// ESP_LOGx() AND plain printf()/puts() call - including console commands and
// esp-idf's own built-in "help" command - is captured. Called by
// co_console_init(), which must run as early as possible in app_main (before
// anything else can log).
//
// This takes ownership of stdout: nothing may call esp_log_set_vprintf(), or
// redo fclose(stdout)/fwopen() afterward, or capture will be silently
// displaced. Route additional output destinations through
// log_ringbuffer_set_downstream() instead.
void log_ringbuffer_init(void);

// The captured stdout stream installed by log_ringbuffer_init(). A task that
// temporarily points its own stdout elsewhere (the serial REPL does, while
// line-editing) switches back to this to have its output captured again.
FILE *log_ringbuffer_stream(void);

// Number of valid lines currently stored (0..LOG_RINGBUFFER_LINES).
size_t log_ringbuffer_count(void);

// Retrieves line `index` in chronological order (0 = oldest, count()-1 =
// newest) into `out` (null-terminated, truncated to out_size). Stored lines
// never carry a trailing '\n'/'\r' so output is consistent whether or not a
// line got truncated - callers add their own line separator back when
// displaying/streaming multiple lines.
// Returns false if index is out of range.
bool log_ringbuffer_get(size_t index, char *out, size_t out_size);

void log_ringbuffer_clear(void);

// Suspends (enabled=false) / resumes (enabled=true) pushing captured stdout
// bytes into the ring buffer, without affecting whether those bytes still
// reach the serial/downstream writers. Nestable. Used by commands whose
// own diagnostic output would otherwise loop back into the buffer it's
// dumping (e.g. "logbuf").
void log_ringbuffer_set_capture_enabled(bool enabled);

// Where captured raw stdout bytes are mirrored to on the chip's console port
// (USB-Serial-JTAG/UART). Owned by co_console; NULL means not mirrored.
void log_ringbuffer_set_serial(raw_writer_t serial);

// Lets ota_logs (or any other future consumer) change where captured raw
// stdout bytes are additionally forwarded to, without displacing the ring
// buffer as the actual stdout sink. Independent of the serial mirror.
// Returns the previous downstream.
raw_writer_t log_ringbuffer_set_downstream(raw_writer_t downstream);
raw_writer_t log_ringbuffer_get_downstream(void);
