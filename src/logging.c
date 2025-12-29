#include "logging.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>

void log_event(const char *file, const char *event) {
    FILE *f = fopen(file, "a");
    if (!f) return;

    flock(fileno(f), LOCK_EX);
    time_t now = time(NULL);
    fprintf(f, "[%ld] PID %d: %s\n", now, getpid(), event);
    fflush(f);
    flock(fileno(f), LOCK_UN);
    fclose(f);

    // Also print to terminal
    printf("%s\n", event);
    fflush(stdout);
}
