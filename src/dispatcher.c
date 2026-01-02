#include "common.h"
#include "logging.h"
#include "ipc.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <signal.h>
#include <stdlib.h>

static volatile int running = 1;

void cleanup(int sig) {
    (void)sig;
    log_dispatcher(LOG_INFO, "Cleaning up and exiting");
    ipc_detach_all();
    ipc_cleanup_all();
    exit(0);
}

int main(void) {
    log_dispatcher(LOG_INFO, "Starting");

    struct sigaction sa = {0};
    sa.sa_handler = cleanup;
    sigaction(SIGINT, &sa, NULL);

    if (ipc_create_all() == -1) {
        log_dispatcher(LOG_ERROR, "Failed to create IPC resources");
        exit(EXIT_FAILURE);
    }

    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        log_dispatcher(LOG_ERROR, "Failed to get shared memory");
        exit(EXIT_FAILURE);
    }

    shm->station_open = 1;
    shm->boarding_allowed = 1;
    shm->passengers_waiting = 0;

    for (int i = 0; i < MAX_BUSES; i++) {
        shm->buses[i].at_station = 1;
        shm->buses[i].passenger_count = 0;
        shm->buses[i].bike_count = 0;
    }

    log_dispatcher(LOG_INFO, "Ready");

    while (running) {
        sleep(DISPATCHER_INTERVAL);
        log_dispatcher(LOG_DEBUG, "Station open: %d, Boarding allowed: %d, Waiting passengers: %d",
               shm->station_open, shm->boarding_allowed, shm->passengers_waiting);
    }

    ipc_detach_all();
    return 0;
}
