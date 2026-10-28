#include <stdio.h>
#include <string.h>
#include "co_console.h"
#include "log_ringbuffer.h"
#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "CANopenNode_ESP32.h"
#include "OD.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#else
#error "co_console supports a USB-Serial-JTAG or UART primary console only"
#endif

static const char *TAG = "console";

// Big enough to absorb a burst of boot/log lines while a host is attached but
// slow to drain; writes beyond it are dropped rather than blocking the logger.
#define SERIAL_TX_BUFFER_SIZE 4096
#define SERIAL_RX_BUFFER_SIZE 256
#define REPL_TASK_STACK_SIZE 4096
#define REPL_TASK_PRIORITY 2
#define MAX_CMDLINE_LENGTH 256

static SemaphoreHandle_t s_run_mutex;

/* ---------------- console port (USB-Serial-JTAG / UART) ---------------- */

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

#define CONSOLE_DEV_PATH "/dev/usbserjtag"

static void serial_install(void)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.tx_buffer_size = SERIAL_TX_BUFFER_SIZE;
    config.rx_buffer_size = SERIAL_RX_BUFFER_SIZE;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_vfs_use_driver();
}

static inline bool serial_connected(void)
{
    return usb_serial_jtag_is_connected();
}

static inline bool serial_put(const char *data, size_t size)
{
    return usb_serial_jtag_write_bytes(data, size, 0) == (int)size;
}

#else // UART

#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM

static char s_console_dev_path[16];
#define CONSOLE_DEV_PATH s_console_dev_path

static void serial_install(void)
{
    snprintf(s_console_dev_path, sizeof(s_console_dev_path), "/dev/uart/%d", CONSOLE_UART);
    // The console UART was already configured (pins, baud) by the bootloader
    // and esp-idf startup; installing the driver keeps that configuration.
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, SERIAL_RX_BUFFER_SIZE, SERIAL_TX_BUFFER_SIZE, 0, NULL, 0));
    uart_vfs_dev_port_set_rx_line_endings(CONSOLE_UART, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONSOLE_UART, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_use_driver(CONSOLE_UART);
}

// A UART has no notion of a host being attached.
static inline bool serial_connected(void)
{
    return true;
}

static inline bool serial_put(const char *data, size_t size)
{
    // Never block the logging task on a full TX buffer: drop instead.
    size_t free_size = 0;
    if (uart_get_tx_buffer_free_size(CONSOLE_UART, &free_size) != ESP_OK || free_size < size)
        return false;
    return uart_write_bytes(CONSOLE_UART, data, size) == (int)size;
}

#endif

#if CONFIG_CO_CONSOLE_SERIAL_OUTPUT
// Mirror of captured stdout onto the console port, with the \n -> \r\n
// translation the VFS driver would otherwise have done. Silently drops output
// while no host is attached or the TX buffer is full.
static int serial_mirror_write(const char *data, int size)
{
    if (!serial_connected())
        return size;

    int start = 0;
    for (int i = 0; i < size; i++)
    {
        if (data[i] != '\n')
            continue;
        if ((i > start && !serial_put(data + start, i - start)) || !serial_put("\r\n", 2))
            return size;
        start = i + 1;
    }
    if (size > start)
        serial_put(data + start, size - start);
    return size;
}
#endif

/* ---------------- command execution ---------------- */

esp_err_t co_console_run(const char *source, const char *line)
{
    int ret;
    // esp_console_run() parses into a single static buffer, so concurrent
    // callers (REPL, SDO 0x2051, TCP console) must not overlap.
    xSemaphoreTake(s_run_mutex, portMAX_DELAY);
    esp_err_t err = esp_console_run(line, &ret);
    xSemaphoreGive(s_run_mutex);

    if (err == ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "%s: unrecognized command: %s", source, line);
    else if (err == ESP_ERR_INVALID_ARG)
        ; // empty command line
    else if (err == ESP_OK && ret != ESP_OK)
        ESP_LOGW(TAG, "%s: '%s' returned 0x%x (%s)", source, line, ret, esp_err_to_name(ret));
    else if (err != ESP_OK)
        ESP_LOGW(TAG, "%s: failed to run '%s': %s", source, line, esp_err_to_name(err));
    return err;
}

/* ---------------- interactive REPL ---------------- */

#if CONFIG_CO_CONSOLE_REPL
static const char *s_prompt;

// Line editing talks to the console port directly (this task's own
// stdin/stdout point at the device), so the prompt, echo and cursor escape
// sequences never reach the log ring buffer. While a command runs, this
// task's stdout/stderr are switched back to the captured stream so its output
// is recorded (and mirrored to the port) like any other log output.
static void repl_task(void *arg)
{
    FILE *dev_in = fopen(CONSOLE_DEV_PATH, "r");
    FILE *dev_out = fopen(CONSOLE_DEV_PATH, "w");
    if (dev_in == NULL || dev_out == NULL)
    {
        ESP_LOGE(TAG, "cannot open %s, REPL disabled", CONSOLE_DEV_PATH);
        vTaskDelete(NULL);
        return;
    }
    setvbuf(dev_in, NULL, _IONBF, 0);
    setvbuf(dev_out, NULL, _IONBF, 0);
    FILE *captured = log_ringbuffer_stream();
    stdin = dev_in;

    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);
    linenoiseHistorySetMaxLen(50);
    linenoiseSetMaxLineLen(MAX_CMDLINE_LENGTH);
    linenoiseAllowEmpty(false);

    bool probed = false;
    while (true)
    {
        stdout = stderr = dev_out;
        if (!serial_connected())
        {
            // Nothing to talk to; re-probe the terminal on the next connect.
            probed = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!probed)
        {
            linenoiseSetDumbMode(linenoiseProbe() != 0);
            probed = true;
        }

        char *line = linenoise(s_prompt);
        if (line == NULL)
        {
            // Empty line, or the host went away mid-read. In dumb mode, re-probe
            // so a terminal opened after connecting gets full line editing once
            // the user presses enter.
            if (linenoiseIsDumbMode())
                probed = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        linenoiseHistoryAdd(line);

        stdout = stderr = captured;
        ESP_LOGI(TAG, "running: %s", line);
        co_console_run("repl", line);
        linenoiseFree(line);
    }
}
#endif

/* ---------------- "logbuf" command ---------------- */

static struct
{
    struct arg_lit *clear;
    struct arg_end *end;
} logbuf_args;

static int do_logbuf(int argc, char **argv)
{
    int n_errors = arg_parse(argc, argv, (void **)&logbuf_args);
    if (n_errors != 0)
    {
        arg_print_errors(stdout, logbuf_args.end, argv[0]);
        return 1;
    }

    if (logbuf_args.clear->count)
    {
        log_ringbuffer_clear();
        log_ringbuffer_set_capture_enabled(false);
        printf("Log buffer cleared\n");
        log_ringbuffer_set_capture_enabled(true);
        return ESP_OK;
    }

    // Capture is suspended around this dump: stdout is captured into the
    // ring buffer, so without this guard, printing the buffer's contents
    // would push its own dump text back into the buffer it's reading from,
    // overwriting real history with an echo of itself.
    log_ringbuffer_set_capture_enabled(false);

    size_t count = log_ringbuffer_count();
    printf("Log buffer: %u/%u lines\n", (unsigned)count, (unsigned)LOG_RINGBUFFER_LINES);

    char line[LOG_RINGBUFFER_LINE_LEN];
    for (size_t i = 0; i < count; i++)
    {
        if (log_ringbuffer_get(i, line, sizeof(line)))
            printf("%3u: %s\n", (unsigned)i, line);
    }

    log_ringbuffer_set_capture_enabled(true);
    return ESP_OK;
}

static void logbuf_register(void)
{
    logbuf_args.clear = arg_lit0("c", "clear", "clear the log ring buffer");
    logbuf_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "logbuf",
        .help = "Show captured log ring buffer (for diagnosis / later SDO retrieval)",
        .hint = NULL,
        .func = &do_logbuf,
        .argtable = &logbuf_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ---------------- OD 0x2050 logbuf / 0x2051 command ---------------- */

// Streams the whole log ring buffer out as one newline-joined blob on SDO
// upload of 0x2050 (DOMAIN, VAR). Snapshotted once at the start of a transfer
// (stream->dataOffset == 0) so a segmented/block upload spanning several
// calls sees a consistent view even if new lines keep arriving mid-transfer.
#define LOG_BUFFER_SNAPSHOT_MAX (LOG_RINGBUFFER_LINES * (LOG_RINGBUFFER_LINE_LEN + 1)) // +1/line for the '\n' joiner

static char log_buffer_snapshot[LOG_BUFFER_SNAPSHOT_MAX];
static size_t log_buffer_snapshot_len;

static ODR_t log_buffer_read(OD_stream_t *stream, void *buf, OD_size_t count, OD_size_t *countRead)
{
    if (stream == NULL || buf == NULL || countRead == NULL)
        return ODR_DEV_INCOMPAT;

    if (stream->dataOffset == 0)
    {
        size_t n = log_ringbuffer_count();
        size_t pos = 0;
        char line[LOG_RINGBUFFER_LINE_LEN];
        for (size_t i = 0; i < n; i++)
        {
            if (!log_ringbuffer_get(i, line, sizeof(line)))
                break;
            size_t line_len = strlen(line);
            if (pos + line_len + 1 >= sizeof(log_buffer_snapshot))
                break; // out of snapshot room, stop early rather than overflow
            memcpy(log_buffer_snapshot + pos, line, line_len);
            pos += line_len;
            log_buffer_snapshot[pos++] = '\n';
        }
        log_buffer_snapshot_len = pos;
    }

    size_t remaining = log_buffer_snapshot_len - stream->dataOffset;
    size_t chunk = remaining < count ? remaining : count;
    memcpy(buf, log_buffer_snapshot + stream->dataOffset, chunk);
    *countRead = chunk;
    stream->dataOffset += chunk;

    return (stream->dataOffset < log_buffer_snapshot_len) ? ODR_PARTIAL : ODR_OK;
}

static OD_extension_t OD_ENTRY_H2050_logbuf_extension = {.object = NULL, .read = log_buffer_read, .write = NULL};

// Runs an SDO-written command through the same esp_console dispatcher used by
// the REPL, as if it had been typed at the prompt. OD_writeOriginal does the
// actual buffering across segmented transfers and null-terminates the string
// (ODA_STR); we just run it once the write is complete.
static ODR_t command_write(OD_stream_t *stream, const void *buf, OD_size_t count, OD_size_t *countWritten)
{
    ODR_t ret = OD_writeOriginal(stream, buf, count, countWritten);
    if (ret != ODR_OK)
        return ret;

    char *command = (char *)stream->dataOrig;
    if (command[0] == '\0')
        return ODR_OK;

    ESP_LOGI(TAG, "sdo running: %s", command);
    // The SDO server calls us with the OD lock held (taken once; the mutex is recursive).
    // Release it while the command runs: commands can block for long or wait on other
    // tasks, and holding it would stall the CANopen periodic (PDO) task or deadlock
    // against a task waiting for the lock. Re-take it so the server's unlock balances.
    CO_UNLOCK_OD(CO->CANmodule);
    co_console_run("sdo", command);
    CO_LOCK_OD(CO->CANmodule);

    command[0] = '\0';
    return ODR_OK;
}

static OD_extension_t OD_ENTRY_H2051_command_extension = {.object = NULL, .read = OD_readOriginal, .write = command_write};

/* ---------------- init ---------------- */

void co_console_init(const char *prompt)
{
    log_ringbuffer_init();

    serial_install();
#if CONFIG_CO_CONSOLE_SERIAL_OUTPUT
    log_ringbuffer_set_serial(serial_mirror_write);
#endif

    s_run_mutex = xSemaphoreCreateMutex();
    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length = MAX_CMDLINE_LENGTH;
    ESP_ERROR_CHECK(esp_console_init(&console_config));
    esp_console_register_help_command();
    logbuf_register();

    OD_extension_init(OD_ENTRY_H2050_logbuf, &OD_ENTRY_H2050_logbuf_extension);
    OD_extension_init(OD_ENTRY_H2051_command, &OD_ENTRY_H2051_command_extension);

#if CONFIG_CO_CONSOLE_REPL
    s_prompt = prompt;
    xTaskCreate(repl_task, "console", REPL_TASK_STACK_SIZE, NULL, REPL_TASK_PRIORITY, NULL);
#endif
}
