#ifndef ECHO_LOGGING_H
#define ECHO_LOGGING_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

void log_init(void);
void log_cleanup(void);
void log_set_level(LogLevel level);

void log_msg(LogLevel level, const char *file, int line,
             const char *message, ...);

#define log_debug(...)  log_msg(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)   log_msg(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)   log_msg(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...)  log_msg(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif
