#include "common.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <signal.h>
#include <stdlib.h>

int shm_id;
int sem_id;
shm_data_t *shm;

void cleanup(int sig) {
    printf("\nDispatch will cleanup, closing the station\n");
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    exit(0);
}


void sem_set(int sem_id, int sem_num, int val) {
    semctl(sem_id, sem_num, SETVAL, val);
}


int main() {

    struct sigaction sa;
    sa.sa_handler = &cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction error SIGINT");
    }

    shm_id = shmget(SHM_KEY, sizeof(shm_data_t), IPC_CREAT | 0666);
    if (shm_id == -1) {
        perror("shmget error");
        exit(1);
    }

    shm = (shm_data_t*) shmat(shm_id, NULL, 0);
    if (shm == (void*) -1) {
        perror("shmat error");
        exit(2);
    }

    shm->station_open = 1; // opening station
    shm->waiting_passengers =0; //on start noone waits yet

    // initializing all the mpk's
    for(int i=0; i<N; i++) {
        shm->bus_present[i] = 1;
        shm->bus_passengers[i] = 0;
        shm->bus_bikes[i] = 0;
    }

    sem_id = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0666);
    if (sem_id == -1) {
        perror("semget error");
        exit(3);
    }

    sem_set(sem_id, SEM_REGISTER, 1);
    sem_set(sem_id, SEM_LOG, 1);
    sem_set(sem_id, SEM_STATION, 1);
    sem_set(sem_id, SEM_GLOBAL, 1);

    printf("All set up, dispatcher ready, SIGINT to cleanup.");

    while(1) {
        printf("\n===BUS STATUS===\n");
        for (int i=0; i<N; i++) {
            printf("BUS nr: %d, availability: %d, passengers: %d, bikes: %d\n",
            i, shm->bus_present[i], shm->bus_passengers[i], shm->bus_bikes[i]);
            printf("--------------------\n");
        }
        printf("=====================\n");
        sleep(10);
    }

    return 0;
}
