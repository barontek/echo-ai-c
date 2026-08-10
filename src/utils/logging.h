/*
 * logging.h - leveled JSON-lines logging to stderr. Depends on: none.
 */

#ifndef ECHO_LOGGING_H
#define ECHO_LOGGING_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/**
 * log_init - initialize the logging subsystem
 *
 * Line-buffers stderr so each JSON log record flushes as a unit. Call
 * once at process startup, before any log_* call.
 *
 * Return: 0 on success; -1 when fd 2 is closed (EBADF). On failure the
 * caller should abort with context, since every subsequent log_* call
 * would write nowhere. Thread-safety: not synchronized; call before any
 * other thread logs.
 */
int log_init(void);

/**
 * log_cleanup - tear down the logging subsystem
 *
 * Currently a no-op reserved for future resource release; kept so
 * callers have a symmetric shutdown point.
 *
 * Return: void; never fails.
 */
void log_cleanup(void);

/**
 * log_set_level - set the global minimum logged level
 * @level: levels below this are dropped (LOG_DEBUG < LOG_INFO <
 *   LOG_WARN < LOG_ERROR). The default is LOG_WARN.
 *
 * Return: void; never fails. Thread-safety: writes a process-wide
 * static; not synchronized — callers must serialize access, typically
 * once at startup.
 */
void log_set_level(LogLevel level);

/**
 * log_msg - write one JSON log record to stderr
 * @level: severity of the record.
 * @file: source file name (macros pass __FILE__).
 * @line: source line (macros pass __LINE__).
 * @message: fixed message string, included verbatim.
 * @...: optional key/value pairs as (const char *key, const char *value)
 *   alternating pairs, terminated by a NULL sentinel. Values are
 *   JSON-escaped (quotes/backslashes) before emission.
 *
 * Records are emitted as one JSON line: {"timestamp":...,"level":...,
 * "file":...,"line":...,"message":"...",key:value,...}. A record with
 * level below the current global level is silently dropped. Records are
 * never buffered beyond the line (see log_init).
 *
 * Prefer the log_debug/log_info/log_warn/log_error macros, which supply
 * file/line automatically.
 *
 * Return: void; never fails. Thread-safety: reads the global level and
 * writes stderr without locking; concurrent calls may interleave lines.
 */
void log_msg(LogLevel level, const char *file, int line,
             const char *message, ...);

#define log_debug(...)  log_msg(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)   log_msg(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)   log_msg(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...)  log_msg(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif
