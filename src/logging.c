#include "logging.h"
#include "ipc.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>

#define MAX_LOG_LINE 512
#define TIME_BUFFER_SIZE 32

typedef enum {
    LOG_MODE_VERBOSE = 0,
    LOG_MODE_SUMMARY = 1,
    LOG_MODE_MINIMAL = 2
} log_mode_t;

static log_mode_t g_log_mode = LOG_MODE_VERBOSE;
static int g_log_mode_initialized = 0;

static int g_perf_mode = 0;
static int g_perf_mode_initialized = 0;

static void init_log_mode_from_env_once(void) {
    if (g_log_mode_initialized) {
        return;
    }
    g_log_mode_initialized = 1;

    const char *mode = getenv("BUS_LOG_MODE");
    if (mode != NULL) {
        if (strcmp(mode, "summary") == 0) {
            g_log_mode = LOG_MODE_SUMMARY;
        } else if (strcmp(mode, "minimal") == 0) {
            g_log_mode = LOG_MODE_MINIMAL;
        } else {
            g_log_mode = LOG_MODE_VERBOSE;
        }
    }
}

static void init_perf_mode_from_env_once(void) {
    if (g_perf_mode_initialized) {
        return;
    }
    g_perf_mode_initialized = 1;

    const char *p = getenv("BUS_PERF_MODE");
    if (p && (strcmp(p, "1") == 0 || strcasecmp(p, "true") == 0 || strcasecmp(p, "yes") == 0)) {
        g_perf_mode = 1;
    } else {
        g_perf_mode = 0;
    }
}

int log_is_perf_mode(void) {
    init_perf_mode_from_env_once();
    return g_perf_mode;
}

static void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

static const char* level_to_string(log_level_t level) {
    switch (level) {
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_DEBUG: return "DEBUG";
        default:        return "UNKNOWN";
    }
}

static void write_log_entry(const char *filename, const char *entry) {
    FILE *f = fopen(filename, "a");
    if (f == NULL) {
        perror("write_log_entry: fopen failed");
        return;
    }

    if (flock(fileno(f), LOCK_EX) == -1) {
        perror("write_log_entry: flock LOCK_EX failed");
        fclose(f);
        return;
    }

    fprintf(f, "%s\n", entry);
    fflush(f);
    if (flock(fileno(f), LOCK_UN) == -1) {
        perror("write_log_entry: flock LOCK_UN failed");
    }

    fclose(f);
}

int log_init(void) {
    if (mkdir(LOG_DIR, 0755) == -1) {
        if (errno != EEXIST) {
            perror("log_init: mkdir failed");
            return -1;
        }
    }

    init_log_mode_from_env_once();

    return 0;
}

void log_event(const char *filename, log_level_t level, const char *format, ...) {
    init_log_mode_from_env_once();
#ifndef DEBUG
    if (level == LOG_DEBUG) {
        return;
    }
#endif

    char timestamp[TIME_BUFFER_SIZE];
    char message[MAX_LOG_LINE];
    char entry[MAX_LOG_LINE + 64];
    va_list args;

    get_timestamp(timestamp, sizeof(timestamp));
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: %s",
             timestamp, level_to_string(level), getpid(), message);

    write_log_entry(filename, entry);
    if (g_log_mode == LOG_MODE_VERBOSE || level >= LOG_WARN) {
        printf("%s\n", entry);
        fflush(stdout);
    }
}

void log_master(log_level_t level, const char *format, ...) {
    init_log_mode_from_env_once();
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_MASTER, level, "%s", message);
}

void log_dispatcher(log_level_t level, const char *format, ...) {
    init_log_mode_from_env_once();
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_DISPATCHER, level, "[DISPATCHER] %s", message);
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [DISPATCHER] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

void log_ticket_office(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_TICKET_OFFICE, level, "[TICKET_OFFICE] %s", message);
    
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [TICKET_OFFICE] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

void log_driver(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_DRIVER, level, "[DRIVER] %s", message);
    
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [DRIVER] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

void log_passenger(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_PASSENGER, level, "[PASSENGER] %s", message);
    
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [PASSENGER] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

void log_stats(const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] %s",
             timestamp, message);
    
    write_log_entry(LOG_STATS, entry);
}

void log_close(void) {
}
