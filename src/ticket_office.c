#include "common.h"
#include "logging.h"
#include "ipc.h"
#include <sys/msg.h>
#include <sys/sem.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    if (ipc_attach_all() == -1) {
        log_ticket_office(LOG_ERROR, "Failed to attach to IPC resources");
        exit(EXIT_FAILURE);
    }

    log_ticket_office(LOG_INFO, "Started");

    ticket_msg_t msg;

    while (1) {
        sem_lock(SEM_TICKET_OFFICE_0);

        ssize_t ret = msg_recv_ticket(&msg, MSG_TICKET_REQUEST, 0);
        if (ret > 0) {
            msg.approved = 1;
            msg.mtype = msg.passenger.pid;
            if (msg_send_ticket(&msg) == -1) {
                log_ticket_office(LOG_ERROR, "Failed to send ticket ack");
            }

            log_ticket_office(LOG_INFO, "Ticket issued for PID %d", msg.passenger.pid);
        }

        sem_unlock(SEM_TICKET_OFFICE_0);
    }

    ipc_detach_all();
    return 0;
}
