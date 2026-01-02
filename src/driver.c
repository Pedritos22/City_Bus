#include "common.h"
#include "logging.h"
#include "ipc.h"
#include <sys/shm.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    srand(time(NULL) ^ getpid());

    if (ipc_attach_all() == -1) {
        log_driver(LOG_ERROR, "Failed to attach to IPC resources");
        exit(EXIT_FAILURE);
    }

    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        log_driver(LOG_ERROR, "Failed to get shared memory");
        exit(EXIT_FAILURE);
    }

    log_driver(LOG_INFO, "Started");

    while (1) {
        sleep(BOARDING_INTERVAL);

        shm->boarding_allowed = 0;
        log_driver(LOG_INFO, "Bus departed");

        sleep(rand() % (MAX_RETURN_TIME - MIN_RETURN_TIME + 1) + MIN_RETURN_TIME);

        for (int i = 0; i < MAX_BUSES; i++) {
            shm->buses[i].passenger_count = 0;
            shm->buses[i].bike_count = 0;
        }
        shm->boarding_allowed = 1;
        log_driver(LOG_INFO, "Bus returned and boarding open");
    }

    ipc_detach_all();
    return 0;
}
