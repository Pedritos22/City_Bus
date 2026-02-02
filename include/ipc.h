#ifndef IPC_H
#define IPC_H

#include "common.h"

int ipc_create_all(void);
int ipc_attach_all(void);
void ipc_detach_all(void);
void ipc_cleanup_all(void);
int ipc_resources_exist(void);

shm_data_t* ipc_get_shm(void);
int ipc_get_shmid(void);

int ipc_get_semid(void);
int sem_lock(int sem_num);
void sem_unlock(int sem_num);
int sem_getval(int sem_num);
void sem_setval(int sem_num, int value);

int ipc_get_msgid_ticket(void);
int ipc_get_msgid_boarding(void);
int ipc_get_msgid_dispatch(void);

int msg_send_ticket(ticket_msg_t *msg);
int msg_send_ticket_resp(ticket_msg_t *msg);
ssize_t msg_recv_ticket(ticket_msg_t *msg, long mtype, int flags);
ssize_t msg_recv_ticket_resp(ticket_msg_t *msg, long mtype, int flags);

int msg_send_boarding(boarding_msg_t *msg);
int msg_send_boarding_resp(boarding_msg_t *msg);
ssize_t msg_recv_boarding(boarding_msg_t *msg, long mtype, int flags);
ssize_t msg_recv_boarding_resp(boarding_msg_t *msg, long mtype, int flags);

int msg_send_dispatch(dispatch_msg_t *msg);
ssize_t msg_recv_dispatch(dispatch_msg_t *msg, long mtype, int flags);

void ipc_check_queue_health(void);

#endif
