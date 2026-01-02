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

    if (ipc_attach_all() == -1) {
        log_passenger(LOG_ERROR, "Failed to attach to IPC resources");
        exit(EXIT_FAILURE);
    }

    passenger_info_t info;
    info.pid = getpid();
    info.has_bike = (rand() % 100) < BIKE_PERCENT;
    info.is_vip = (rand() % 100) < VIP_PERCENT;
    info.age = rand() % (MAX_AGE - MIN_AGE + 1) + MIN_AGE;
    info.has_ticket = info.is_vip ? 1 : 0;
    info.is_child = IS_CHILD(info.age);
    info.seat_count = 1;

    if (!info.is_vip) {
        ticket_msg_t msg;
        msg.mtype = MSG_TICKET_REQUEST;
        msg.passenger = info;

        if (msg_send_ticket(&msg) == -1) {
            log_passenger(LOG_ERROR, "Failed to send ticket request");
            exit(EXIT_FAILURE);
        }

        ticket_msg_t ack;
        if (msg_recv_ticket(&ack, getpid(), 0) == -1) {
            log_passenger(LOG_ERROR, "Failed to receive ticket ack");
            exit(EXIT_FAILURE);
        }
        info.has_ticket = ack.approved;
    }

    log_passenger(LOG_INFO, "PID %d ready to board (VIP=%d, Age=%d, Bike=%d)",
           getpid(), info.is_vip, info.age, info.has_bike);

    sleep(rand() % 3 + 1);

    log_passenger(LOG_INFO, "PID %d boarded", getpid());

    ipc_detach_all();
    return 0;
}
