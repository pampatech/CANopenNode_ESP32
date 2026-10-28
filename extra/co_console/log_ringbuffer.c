#include <string.h>
#include <stdio.h>
#include "log_ringbuffer.h"
#include "freertos/FreeRTOS.h"

static char s_lines[LOG_RINGBUFFER_LINES][LOG_RINGBUFFER_LINE_LEN];
static size_t s_tail;  // index of oldest valid line
static size_t s_head;  // index where the next line will be written
static size_t s_count; // number of valid lines currently stored

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static FILE *s_stream;
static raw_writer_t s_serial;
static raw_writer_t s_downstream;
static volatile int s_capture_suppress_depth;

static char s_accum[LOG_RINGBUFFER_LINE_LEN];
static size_t s_accum_len;

static void log_ringbuffer_push(const char *line)
{
    portENTER_CRITICAL(&s_lock);
    memcpy(s_lines[s_head], line, LOG_RINGBUFFER_LINE_LEN);
    s_head = (s_head + 1) % LOG_RINGBUFFER_LINES;
    if (s_count < LOG_RINGBUFFER_LINES)
        s_count++;
    else
        s_tail = (s_tail + 1) % LOG_RINGBUFFER_LINES; // full: drop the oldest
    portEXIT_CRITICAL(&s_lock);
}

// Reassembles raw stdout bytes into lines, pushing each completed line into
// the ring. Overflow beyond LOG_RINGBUFFER_LINE_LEN-1 is silently truncated,
// matching the clamp behavior of the previous vsnprintf-based capture.
//
// Correctness across concurrent tasks relies on esp-idf's newlib holding a
// per-FILE* lock for the duration of one logical printf()/vfprintf() call,
// so two tasks' output bytes can't interleave mid-line here. A violation
// would only corrupt stored history cosmetically (log_ringbuffer_push itself
// stays correctly locked) - verify with two tasks logging concurrently if in
// doubt.
static void log_ringbuffer_accumulate(const char *data, int size)
{
    for (int i = 0; i < size; i++)
    {
        char c = data[i];
        if (c == '\n' || c == '\r')
        {
            if (s_accum_len > 0)
            {
                s_accum[s_accum_len] = '\0';
                log_ringbuffer_push(s_accum);
                s_accum_len = 0;
            }
            continue;
        }
        if (s_accum_len < sizeof(s_accum) - 1)
            s_accum[s_accum_len++] = c;
    }
}

static int log_ringbuffer_write(void *cookie, const char *data, int size)
{
    if (s_capture_suppress_depth == 0)
        log_ringbuffer_accumulate(data, size);

    raw_writer_t serial = s_serial;
    if (serial)
        serial(data, size);
    raw_writer_t downstream = s_downstream;
    if (downstream)
        downstream(data, size);
    return size;
}

void log_ringbuffer_init(void)
{
    fclose(stdout);
    s_stream = fwopen(NULL, &log_ringbuffer_write);
    _GLOBAL_REENT->_stdout = s_stream;
    stdout = s_stream;
    setvbuf(stdout, NULL, _IONBF, 0);
}

FILE *log_ringbuffer_stream(void)
{
    return s_stream;
}

size_t log_ringbuffer_count(void)
{
    return s_count;
}

bool log_ringbuffer_get(size_t index, char *out, size_t out_size)
{
    if (index >= s_count)
        return false;

    portENTER_CRITICAL(&s_lock);
    size_t actual_index = (s_tail + index) % LOG_RINGBUFFER_LINES;
    snprintf(out, out_size, "%s", s_lines[actual_index]);
    portEXIT_CRITICAL(&s_lock);
    return true;
}

void log_ringbuffer_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_tail = 0;
    s_head = 0;
    s_count = 0;
    portEXIT_CRITICAL(&s_lock);
}

void log_ringbuffer_set_capture_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_capture_suppress_depth += enabled ? -1 : 1;
    portEXIT_CRITICAL(&s_lock);
}

void log_ringbuffer_set_serial(raw_writer_t serial)
{
    s_serial = serial;
}

raw_writer_t log_ringbuffer_set_downstream(raw_writer_t downstream)
{
    raw_writer_t previous = s_downstream;
    s_downstream = downstream;
    return previous;
}

raw_writer_t log_ringbuffer_get_downstream(void)
{
    return s_downstream;
}
