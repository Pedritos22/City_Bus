#ifndef LOGGING_H
#define LOGGING_H

#include "config.h"

typedef enum {
    LOG_INFO = 0,
    LOG_WARN,
    LOG_ERROR,
    LOG_DEBUG // DEBUG messages
} log_level_t;

// ANSI color codes (log files stay plain)
#define COLOR_RESET "\033[0m"
#define COLOR_RED   "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE  "\033[34m"
#define COLOR_CYAN  "\033[36m"

// Creates log directory if it doesn't exist.
int log_init(void);
// Log a formatted event to a specific file.
void log_event(const char *filename, log_level_t level, const char *format, ...);
// Specific log functions event handler for different files.
void log_master(log_level_t level, const char *format, ...);
void log_dispatcher(log_level_t level, const char *format, ...);
void log_ticket_office(log_level_t level, const char *format, ...);
void log_driver(log_level_t level, const char *format, ...);
void log_passenger(log_level_t level, const char *format, ...);
void log_stats(const char *format, ...);
int log_is_perf_mode(void);
void log_close(void);

#endif
