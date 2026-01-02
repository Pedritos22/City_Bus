#include "config.h"
#include "logging.h"
#include "ipc.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(void) {
    log_init(); // create logs dir if does not exist

    log_master(LOG_INFO, "Simulation starting");

    pid_t pid;

    // dispatcher
    pid = fork();
    if (pid == 0) {
        execl("./dispatcher", "dispatcher", NULL);
        perror("execl dispatcher failed");
        exit(EXIT_FAILURE);
    }
    log_master(LOG_INFO, "Dispatcher started with PID %d", pid);

    // ticket office
    pid = fork();
    if (pid == 0) {
        execl("./ticket_office", "ticket_office", NULL);
        perror("execl ticket_office failed");
        exit(EXIT_FAILURE);
    }
    log_master(LOG_INFO, "Ticket office started with PID %d", pid);

    // driver
    pid = fork();
    if (pid == 0) {
        execl("./driver", "driver", NULL);
        perror("execl driver failed");
        exit(EXIT_FAILURE);
    }
    log_master(LOG_INFO, "Driver started with PID %d", pid);

    // wait for ipcs to be ready
    sleep(1);

    // passengers
    for (int i = 0; i < MAX_PASSENGERS; i++) {
        pid = fork();
        if (pid == 0) {
            execl("./passenger", "passenger", NULL);
            perror("execl passenger failed");
            exit(EXIT_FAILURE);
        }
        log_master(LOG_DEBUG, "Passenger %d started with PID %d", i, pid);
        usleep(500000); // half a sec between pass
    }

    // waiting for the children
    for (int i = 0; i < MAX_PASSENGERS + 3; i++) wait(NULL);

    log_master(LOG_INFO, "Simulation finished");
    printf("Simulation finished.\n");
    return 0;
}
