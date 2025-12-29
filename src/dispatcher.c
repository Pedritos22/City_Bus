#include "common.h"
#include "logging.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <signal.h>
#include <stdlib.h>

int shm_id;
int sem_id;
shm_data_t *shm;

void cleanup(int sig) {
    (void)sig;
    printf("[DISPATCHER] Cleaning up and exiting\n");
    if (shm) shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);
    if (sem_id != -1) semctl(sem_id, 0, IPC_RMID);
    exit(0);
}

void sem_set(int sem_id, int sem_num, int val) {
    if (semctl(sem_id, sem_num, SETVAL, val) == -1) {
        perror("semctl SETVAL");
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    printf("[DISPATCHER] Starting\n");

    struct sigaction sa = {0};
    sa.sa_handler = cleanup;
    sigaction(SIGINT, &sa, NULL);

    shm_id = shmget(SHM_KEY, sizeof(shm_data_t), IPC_CREAT | 0600);
    if (shm_id == -1) { perror("shmget"); exit(1); }

    shm = shmat(shm_id, NULL, 0);
    if (shm == (void *)-1) { perror("shmat"); exit(2); }

    shm->station_open = 1;
    shm->boarding_allowed = 1;
    shm->waiting_passengers = 0;

    for (int i = 0; i < MAX_BUSES; i++) {
        shm->bus_present[i] = 1;
        shm->bus_passengers[i] = 0;
        shm->bus_bikes[i] = 0;
    }

    sem_id = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (sem_id == -1) { perror("semget"); exit(3); }

    sem_set(sem_id, SEM_REGISTER, 1);
    sem_set(sem_id, SEM_LOG, 1);
    sem_set(sem_id, SEM_STATION, 1);
    sem_set(sem_id, SEM_ENTRANCE_PASS, 1);
    sem_set(sem_id, SEM_ENTRANCE_BIKE, 1);

    printf("[DISPATCHER] Ready\n");

    while (1) {
        sleep(2);
        printf("[DISPATCHER] Station open: %d, Boarding allowed: %d, Waiting passengers: %d\n",
               shm->station_open, shm->boarding_allowed, shm->waiting_passengers);
        fflush(stdout);
    }
}
