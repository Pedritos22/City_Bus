#include "common.h"
#include "logging.h"
#include "ipc.h"
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

int main(void) {
    srand(time(NULL) ^ getpid());

    int msgid = msgget(MSG_KEY, IPC_CREAT | 0600);
    if (msgid == -1) { perror("msgget"); exit(1); }

    int sem_id = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (sem_id == -1) { perror("semget"); exit(1); }

    passenger_msg_t msg;
    msg.mtype = 1;
    msg.pid = getpid();
    msg.has_bike = rand() % 2;
    msg.is_vip = (rand() % 100) < VIP_PERCENT;
    msg.age = rand() % 70 + 5;
    msg.ticket_issued = msg.is_vip ? 1 : 0;

    if (!msg.is_vip) {
        if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
            perror("msgsnd request");
            exit(1);
        }

        passenger_msg_t ack;
        if (msgrcv(msgid, &ack, sizeof(ack) - sizeof(long), getpid(), 0) == -1) {
            perror("msgrcv ack");
            exit(1);
        }
    }

    printf("[PASSENGER] PID %d ready to board (VIP=%d, Age=%d, Bike=%d)\n",
           getpid(), msg.is_vip, msg.age, msg.has_bike);
    fflush(stdout);

    sleep(rand() % 3 + 1);

    printf("[PASSENGER] PID %d boarded\n", getpid());
    fflush(stdout);

    return 0;
}
