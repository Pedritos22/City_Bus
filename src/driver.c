#include "common.h"
#include "logging.h"
#include <sys/shm.h>
#include <unistd.h>
#include <stdlib.h>

int main(void) {
    int shm_id = shmget(SHM_KEY, sizeof(shm_data_t), 0);
    shm_data_t *shm = shmat(shm_id, NULL, 0);

    printf("[DRIVER] Started\n");

    while (1) {
        sleep(BOARDING_INTERVAL);

        shm->boarding_allowed = 0;
        log_event("logs/driver.log", "[DRIVER] Bus departed");

        sleep(rand()%5 + 1);

        for(int i=0;i<MAX_BUSES;i++) {
            shm->bus_passengers[i] = 0;
            shm->bus_bikes[i] = 0;
        }
        shm->boarding_allowed = 1;
        log_event("logs/driver.log", "[DRIVER] Bus returned and boarding open");
    }
}
