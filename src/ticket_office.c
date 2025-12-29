#include "common.h"
#include "logging.h"
#include "ipc.h"
#include <sys/msg.h>
#include <sys/sem.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    int msgid = msgget(MSG_KEY, IPC_CREAT | 0600);
    if (msgid == -1) { perror("msgget"); exit(1); }

    int sem_id = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (sem_id == -1) { perror("semget"); exit(1); }

    passenger_msg_t msg;
    printf("[TICKET_OFFICE] Started\n");
    fflush(stdout);

    while (1) {
        sem_lock(sem_id, SEM_REGISTER);
        if (msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1, 0) > 0) {
            msg.ticket_issued = 1;
            msg.mtype = msg.pid;
            if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1) perror("msgsnd ack");

            printf("[TICKET_OFFICE] Ticket issued for PID %d\n", msg.pid);
            log_event("logs/ticket_office.log", "[TICKET_OFFICE] Ticket issued");
            fflush(stdout);
        }
        sem_unlock(sem_id, SEM_REGISTER);
    }
}
