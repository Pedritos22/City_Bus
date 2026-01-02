#include "logging.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>

#define MAX_LOG_LINE 512           // Maximum size of a single log message
#define TIME_BUFFER_SIZE 32        // Buffer size for timestamp strings

// Fill buffer with current timestamp
static void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);    // Thread-safe local time
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

// Convert log level enum to a readable string
static const char* level_to_string(log_level_t level) {
    switch (level) {
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_DEBUG: return "DEBUG";
        default:        return "UNKNOWN";
    }
}

// Append a log entry to the specified file with lock
static void write_log_entry(const char *filename, const char *entry) {
    FILE *f = fopen(filename, "a");
    if (f == NULL) {
        perror("write_log_entry: fopen failed"); // File open error
        return;
    }

    if (flock(fileno(f), LOCK_EX) == -1) {     // Lock file to prevent concurrent writes
        perror("write_log_entry: flock LOCK_EX failed");
        fclose(f);
        return;
    }

    fprintf(f, "%s\n", entry);
    fflush(f);

    if (flock(fileno(f), LOCK_UN) == -1) {     // Unlock file
        perror("write_log_entry: flock LOCK_UN failed");
    }

    fclose(f);      
}

// Initialize logging system by creating log directory if it doesn't exist
int log_init(void) {
    if (mkdir(LOG_DIR, 0755) == -1) {
        if (errno != EEXIST) {                 // Ignore error if directory already exists
            perror("log_init: mkdir failed");
            return -1;
        }
    }
    return 0;
}

// General-purpose logging function with timestamp, PID, and log level
void log_event(const char *filename, log_level_t level, const char *format, ...) {
#ifndef DEBUG
    if (level == LOG_DEBUG) {   // Skip debug logs if not in debug mode
        return;
    }
#endif

    char timestamp[TIME_BUFFER_SIZE];
    char message[MAX_LOG_LINE];
    char entry[MAX_LOG_LINE + 64];
    va_list args;

    get_timestamp(timestamp, sizeof(timestamp));   // Get current timestamp

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); // Format variable arguments
    va_end(args);

    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: %s",
             timestamp, level_to_string(level), getpid(), message); // Create final log entry

    write_log_entry(filename, entry);           // Write entry to file

    printf("%s\n", entry);
    fflush(stdout);
}

// Logging wrapper for master process
void log_master(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); // Format master log message
    va_end(args);

    log_event(LOG_MASTER, level, "%s", message);       // Log to master file
}

// Logging wrapper for dispatcher process
void log_dispatcher(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); // Format dispatcher log
    va_end(args);

    log_event(LOG_DISPATCHER, level, "[DISPATCHER] %s", message); // Write to dispatcher log

    // Also log dispatcher message to master log
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));

    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [DISPATCHER] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

// Logging wrapper for ticket office process
void log_ticket_office(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_TICKET_OFFICE, level, "[TICKET_OFFICE] %s", message); // Write to ticket office log

    // Also forward to master log
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));

    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [TICKET_OFFICE] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

// Logging wrapper for driver process
void log_driver(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_DRIVER, level, "[DRIVER] %s", message); // Write to driver log

    // Also forward to master log
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));

    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [DRIVER] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

// Logging wrapper for passenger process
void log_passenger(log_level_t level, const char *format, ...) {
    char message[MAX_LOG_LINE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_event(LOG_PASSENGER, level, "[PASSENGER] %s", message); // Write to passenger log

    // Also forward to master log
    char timestamp[TIME_BUFFER_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));

    char entry[MAX_LOG_LINE + 64];
    snprintf(entry, sizeof(entry), "[%s] [%s] PID=%d: [PASSENGER] %s",
             timestamp, level_to_string(level), getpid(), message);
    write_log_entry(LOG_MASTER, entry);
}

// Placeholder for cleanup
void log_close(void) {
}

