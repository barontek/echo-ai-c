#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include "logging.h"

static LogLevel current_level = LOG_INFO;

static const char *level_str(LogLevel level)
{
    switch (level)
    {
    case LOG_DEBUG: return "debug";
    case LOG_INFO:  return "info";
    case LOG_WARN:  return "warn";
    case LOG_ERROR: return "error";
    default:        return "unknown";
    }
}

void log_init(void)
{
    setlinebuf(stderr);
}

void log_cleanup(void)
{
}

void log_set_level(LogLevel level)
{
    current_level = level;
}

void log_msg(LogLevel level, const char *file, int line,
             const char *message, ...)
{
    if (level < current_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    char timestamp[64];
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm);

    fprintf(stderr, "{\"timestamp\":\"%s.%03ldZ\",\"level\":\"%s\",\"file\":\"%s\",\"line\":%d,\"message\":\"",
            timestamp, ts.tv_nsec / 1000000, level_str(level), file, line);

    va_list args;
    va_start(args, message);

    const char *key = message;
    const char *val = NULL;

    fprintf(stderr, "%s", message);

    for (int i = 0; i < 100; i++)
    {
        key = va_arg(args, const char *);
        if (!key) break;
        val = va_arg(args, const char *);
        if (!val) break;
        fprintf(stderr, "\",\"%s\":\"", key);
        for (const char *p = val; *p; p++)
        {
            if (*p == '"' || *p == '\\') putc('\\', stderr);
            putc(*p, stderr);
        }
    }

    va_end(args);

    fprintf(stderr, "\"}\n");
}
