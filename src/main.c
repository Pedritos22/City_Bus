#include "config.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(void) {
    mkdir("logs", 0755); // create logs dir if does not exist

    pid_t pid;

    // dispatcher
    pid = fork();
    if (pid == 0) {
        execl("./dispatcher", "dispatcher", NULL);
        perror("execl dispatcher failed");
        exit(EXIT_FAILURE);
    }

    // ticket office
    pid = fork();
    if (pid == 0) {
        execl("./ticket_office", "ticket_office", NULL);
        perror("execl ticket_office failed");
        exit(EXIT_FAILURE);
    }

    // driver
    pid = fork();
    if (pid == 0) {
        execl("./driver", "driver", NULL);
        perror("execl driver failed");
        exit(EXIT_FAILURE);
    }

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
        usleep(500000); // half a sec between pass
    }

    // waiting for the children
    for (int i = 0; i < MAX_PASSENGERS + 3; i++) wait(NULL);

    printf("Simulation finished.\n");
    return 0;
}
