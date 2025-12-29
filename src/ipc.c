#include "ipc.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

int create_ipc() {
    int shmid = shmget(SHM_KEY, sizeof(shm_data_t), IPC_CREAT | 0600);
    if (shmid == -1) { perror("shmget"); return -1; }

    int semid = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (semid == -1) { perror("semget"); return -1; }

    int msgid = msgget(MSG_KEY, IPC_CREAT | 0600);
    if (msgid == -1) { perror("msgget"); return -1; }

    // initialize semaph.
    for (int i = 0; i < SEM_COUNT; i++) {
        if (semctl(semid, i, SETVAL, 1) == -1) {
            perror("semctl SETVAL");
            return -1;
        }
    }

    return 0;
}

void cleanup_ipc() {
    shmctl(shmget(SHM_KEY, 0, 0), IPC_RMID, NULL);
    semctl(semget(SEM_KEY, 0, 0), 0, IPC_RMID);
    msgctl(msgget(MSG_KEY, 0), IPC_RMID, NULL);
}

// semaphore helpers
void sem_lock(int sem_id, int sem_num) {
    struct sembuf op = { sem_num, -1, 0 };
    if (semop(sem_id, &op, 1) == -1) { perror("semop lock"); exit(EXIT_FAILURE); }
}

void sem_unlock(int sem_id, int sem_num) {
    struct sembuf op = { sem_num, 1, 0 };
    if (semop(sem_id, &op, 1) == -1) { perror("semop unlock"); exit(EXIT_FAILURE); }
}
