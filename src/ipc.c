#include "ipc.h"
#include "config.h"
#include "logging.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static int g_shmid = -1;
static int g_semid = -1;
static int g_msgid_ticket = -1;
static int g_msgid_ticket_resp = -1;
static int g_msgid_boarding = -1;
static int g_msgid_boarding_resp = -1;
static int g_msgid_dispatch = -1;
static shm_data_t *g_shm = NULL;

#if defined(__linux__)
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};
#elif defined(__APPLE__)
#endif

static void ipc_cleanup_partial(void) {
    /* Cleanup any resources created so far on partial failure */
    if (g_shm != NULL && g_shm != (void *)-1) {
        shmdt(g_shm);
        g_shm = NULL;
    }
    if (g_shmid != -1) {
        shmctl(g_shmid, IPC_RMID, NULL);
        g_shmid = -1;
    }
    if (g_semid != -1) {
        semctl(g_semid, 0, IPC_RMID);
        g_semid = -1;
    }
    if (g_msgid_ticket != -1) {
        msgctl(g_msgid_ticket, IPC_RMID, NULL);
        g_msgid_ticket = -1;
    }
    if (g_msgid_ticket_resp != -1) {
        msgctl(g_msgid_ticket_resp, IPC_RMID, NULL);
        g_msgid_ticket_resp = -1;
    }
    if (g_msgid_boarding != -1) {
        msgctl(g_msgid_boarding, IPC_RMID, NULL);
        g_msgid_boarding = -1;
    }
    if (g_msgid_boarding_resp != -1) {
        msgctl(g_msgid_boarding_resp, IPC_RMID, NULL);
        g_msgid_boarding_resp = -1;
    }
    if (g_msgid_dispatch != -1) {
        msgctl(g_msgid_dispatch, IPC_RMID, NULL);
        g_msgid_dispatch = -1;
    }
}

int ipc_create_all(void) {
    g_shmid = shmget(SHM_KEY, sizeof(shm_data_t), IPC_CREAT | 0600);
    if (g_shmid == -1) {
        perror("ipc_create_all: shmget failed");
        return -1;
    }

    g_shm = (shm_data_t *)shmat(g_shmid, NULL, 0);
    if (g_shm == (void *)-1) {
        perror("ipc_create_all: shmat failed");
        g_shm = NULL;
        ipc_cleanup_partial();
        return -1;
    }

    memset(g_shm, 0, sizeof(shm_data_t));
    g_semid = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (g_semid == -1) {
        perror("ipc_create_all: semget failed");
        ipc_cleanup_partial();
        return -1;
    }

    union semun arg;
    arg.val = 1;
    if (semctl(g_semid, SEM_SHM_MUTEX, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_SHM_MUTEX failed");
        ipc_cleanup_partial();
        return -1;
    }
    if (semctl(g_semid, SEM_LOG_MUTEX, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_LOG_MUTEX failed");
        ipc_cleanup_partial();
        return -1;
    }
    if (semctl(g_semid, SEM_STATION_ENTRY, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_STATION_ENTRY failed");
        ipc_cleanup_partial();
        return -1;
    }
    if (semctl(g_semid, SEM_ENTRANCE_PASSENGER, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_ENTRANCE_PASSENGER failed");
        ipc_cleanup_partial();
        return -1;
    }
    if (semctl(g_semid, SEM_ENTRANCE_BIKE, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_ENTRANCE_BIKE failed");
        ipc_cleanup_partial();
        return -1;
    }
    if (semctl(g_semid, SEM_BOARDING_MUTEX, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_BOARDING_MUTEX failed");
        ipc_cleanup_partial();
        return -1;
    }
    
    arg.val = 0;
    if (semctl(g_semid, SEM_BUS_READY, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_BUS_READY failed");
        ipc_cleanup_partial();
        return -1;
    }
    
    arg.val = 1;
    for (int i = 0; i < TICKET_OFFICES; i++) {
        int sem_idx = SEM_TICKET_OFFICE(i);
        if (semctl(g_semid, sem_idx, SETVAL, arg) == -1) {
            fprintf(stderr, "ipc_create_all: semctl SEM_TICKET_OFFICE_%d failed\n", i);
            perror("semctl");
            ipc_cleanup_partial();
            return -1;
        }
    }

    arg.val = MAX_TICKET_QUEUE_REQUESTS;
    if (semctl(g_semid, SEM_TICKET_QUEUE_SLOTS, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_TICKET_QUEUE_SLOTS failed");
        ipc_cleanup_partial();
        return -1;
    }
    arg.val = MAX_BOARDING_QUEUE_REQUESTS;
    if (semctl(g_semid, SEM_BOARDING_QUEUE_SLOTS, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_BOARDING_QUEUE_SLOTS failed");
        ipc_cleanup_partial();
        return -1;
    }

    g_msgid_ticket = msgget(MSG_TICKET_KEY, IPC_CREAT | 0600);
    if (g_msgid_ticket == -1) {
        perror("ipc_create_all: msgget ticket failed");
        ipc_cleanup_partial();
        return -1;
    }

    /* Separate queue for ticket responses - guarantees responses always have room */
    g_msgid_ticket_resp = msgget(MSG_TICKET_RESP_KEY, IPC_CREAT | 0600);
    if (g_msgid_ticket_resp == -1) {
        perror("ipc_create_all: msgget ticket_resp failed");
        ipc_cleanup_partial();
        return -1;
    }

    g_msgid_boarding = msgget(MSG_BOARDING_KEY, IPC_CREAT | 0600);
    if (g_msgid_boarding == -1) {
        perror("ipc_create_all: msgget boarding failed");
        ipc_cleanup_partial();
        return -1;
    }

    /* Separate queue for boarding responses - guarantees responses always have room */
    g_msgid_boarding_resp = msgget(MSG_BOARDING_RESP_KEY, IPC_CREAT | 0600);
    if (g_msgid_boarding_resp == -1) {
        perror("ipc_create_all: msgget boarding_resp failed");
        ipc_cleanup_partial();
        return -1;
    }

    g_msgid_dispatch = msgget(MSG_DISPATCH_KEY, IPC_CREAT | 0600);
    if (g_msgid_dispatch == -1) {
        perror("ipc_create_all: msgget dispatch failed");
        ipc_cleanup_partial();
        return -1;
    }

    return 0;
}

int ipc_attach_all(void) {
    g_shmid = shmget(SHM_KEY, sizeof(shm_data_t), 0600);
    if (g_shmid == -1) {
        perror("ipc_attach_all: shmget failed");
        return -1;
    }

    g_shm = (shm_data_t *)shmat(g_shmid, NULL, 0);
    if (g_shm == (void *)-1) {
        perror("ipc_attach_all: shmat failed");
        g_shm = NULL;
        return -1;
    }

    g_semid = semget(SEM_KEY, SEM_COUNT, 0600);
    if (g_semid == -1) {
        perror("ipc_attach_all: semget failed");
        return -1;
    }

    g_msgid_ticket = msgget(MSG_TICKET_KEY, 0600);
    if (g_msgid_ticket == -1) {
        perror("ipc_attach_all: msgget ticket failed");
        return -1;
    }

    g_msgid_ticket_resp = msgget(MSG_TICKET_RESP_KEY, 0600);
    if (g_msgid_ticket_resp == -1) {
        perror("ipc_attach_all: msgget ticket_resp failed");
        return -1;
    }

    g_msgid_boarding = msgget(MSG_BOARDING_KEY, 0600);
    if (g_msgid_boarding == -1) {
        perror("ipc_attach_all: msgget boarding failed");
        return -1;
    }

    g_msgid_boarding_resp = msgget(MSG_BOARDING_RESP_KEY, 0600);
    if (g_msgid_boarding_resp == -1) {
        perror("ipc_attach_all: msgget boarding_resp failed");
        return -1;
    }

    g_msgid_dispatch = msgget(MSG_DISPATCH_KEY, 0600);
    if (g_msgid_dispatch == -1) {
        perror("ipc_attach_all: msgget dispatch failed");
        return -1;
    }

    return 0;
}

void ipc_detach_all(void) {
    if (g_shm != NULL && g_shm != (void *)-1) {
        if (shmdt(g_shm) == -1) {
            perror("ipc_detach_all: shmdt failed");
        }
        g_shm = NULL;
    }
}

void ipc_cleanup_all(void) {
    /* Try to get IDs by key if not already set (fallback for main process) */
    int shmid = g_shmid;
    int semid = g_semid;
    int msgid_ticket = g_msgid_ticket;
    int msgid_ticket_resp = g_msgid_ticket_resp;
    int msgid_boarding = g_msgid_boarding;
    int msgid_boarding_resp = g_msgid_boarding_resp;
    int msgid_dispatch = g_msgid_dispatch;

    if (shmid == -1) shmid = shmget(SHM_KEY, 0, 0);
    if (semid == -1) semid = semget(SEM_KEY, 0, 0);
    if (msgid_ticket == -1) msgid_ticket = msgget(MSG_TICKET_KEY, 0);
    if (msgid_ticket_resp == -1) msgid_ticket_resp = msgget(MSG_TICKET_RESP_KEY, 0);
    if (msgid_boarding == -1) msgid_boarding = msgget(MSG_BOARDING_KEY, 0);
    if (msgid_boarding_resp == -1) msgid_boarding_resp = msgget(MSG_BOARDING_RESP_KEY, 0);
    if (msgid_dispatch == -1) msgid_dispatch = msgget(MSG_DISPATCH_KEY, 0);

    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
        g_shmid = -1;
    }

    if (semid != -1) {
        semctl(semid, 0, IPC_RMID);
        g_semid = -1;
    }

    if (msgid_ticket != -1) {
        msgctl(msgid_ticket, IPC_RMID, NULL);
        g_msgid_ticket = -1;
    }

    if (msgid_ticket_resp != -1) {
        msgctl(msgid_ticket_resp, IPC_RMID, NULL);
        g_msgid_ticket_resp = -1;
    }

    if (msgid_boarding != -1) {
        msgctl(msgid_boarding, IPC_RMID, NULL);
        g_msgid_boarding = -1;
    }

    if (msgid_boarding_resp != -1) {
        msgctl(msgid_boarding_resp, IPC_RMID, NULL);
        g_msgid_boarding_resp = -1;
    }

    if (msgid_dispatch != -1) {
        msgctl(msgid_dispatch, IPC_RMID, NULL);
        g_msgid_dispatch = -1;
    }
}

int ipc_resources_exist(void) {
    int shmid = shmget(SHM_KEY, 0, 0);
    return (shmid != -1);
}

shm_data_t* ipc_get_shm(void) {
    return g_shm;
}

int ipc_get_shmid(void) {
    return g_shmid;
}

int ipc_get_semid(void) {
    return g_semid;
}

int sem_lock(int sem_num) {
    if (g_semid == -1) {
        return -1;  /* Semaphore set not initialized or already removed */
    }
    
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = 0;

    while (1) {
        if (semop(g_semid, &op, 1) == 0) {
            return 0;  /* Success */
        }
        
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT from Ctrl+Z/fg) - retry */
            continue;
        }
        
        if (errno == EIDRM || errno == EINVAL) {
            /* Semaphore set removed - simulation ending */
            return -1;
        }
        
        perror("sem_lock: semop failed");
        exit(EXIT_FAILURE);
    }
}

void sem_unlock(int sem_num) {
    if (g_semid == -1) {
        return;  /* Semaphore set not initialized or already removed */
    }
    
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;
    op.sem_flg = 0;

    if (semop(g_semid, &op, 1) == -1) {
        if (errno != EINTR && errno != EIDRM && errno != EINVAL && errno != ERANGE) {
            perror("sem_unlock: semop failed");
            exit(EXIT_FAILURE);
        }
    }
}

int sem_getval(int sem_num) {
    if (g_semid == -1) {
        return 0;  /* Semaphore set not initialized or already removed */
    }
    
    int val = semctl(g_semid, sem_num, GETVAL);
    if (val == -1) {
        if (errno != EIDRM && errno != EINVAL) {
            perror("sem_getval: semctl failed");
        }
        return 0;
    }
    return val;
}

void sem_setval(int sem_num, int value) {
    if (g_semid == -1) {
        return;  /* Semaphore set not initialized or already removed */
    }
    
    union semun arg;
    arg.val = value;
    
    if (semctl(g_semid, sem_num, SETVAL, arg) == -1) {
        if (errno != EIDRM && errno != EINVAL) {
            perror("sem_setval: semctl failed");
        }
    }
}

int ipc_get_msgid_ticket(void) {
    return g_msgid_ticket;
}

int ipc_get_msgid_boarding(void) {
    return g_msgid_boarding;
}

int ipc_get_msgid_dispatch(void) {
    return g_msgid_dispatch;
}

/* 
 * Separate queues for requests and responses guarantee responses always have room.
 * Requests: limited by semaphores
 * Responses: go to dedicated queue, never compete with requests
 */

int msg_send_ticket(ticket_msg_t *msg) {
    while (1) {
        if (msgsnd(g_msgid_ticket, msg, sizeof(ticket_msg_t) - sizeof(long), 0) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != EIDRM) {
            perror("msg_send_ticket: msgsnd failed");
        }
        return -1;
    }
}

/* Send ticket response to separate response queue */
int msg_send_ticket_resp(ticket_msg_t *msg) {
    while (1) {
        if (msgsnd(g_msgid_ticket_resp, msg, sizeof(ticket_msg_t) - sizeof(long), 0) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != EIDRM) {
            perror("msg_send_ticket_resp: msgsnd failed");
        }
        return -1;
    }
}

ssize_t msg_recv_ticket(ticket_msg_t *msg, long mtype, int flags) {
    ssize_t ret;
    while (1) {
        ret = msgrcv(g_msgid_ticket, msg, sizeof(ticket_msg_t) - sizeof(long), mtype, flags);
        if (ret >= 0) {
            return ret;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != ENOMSG && errno != EIDRM) {
            perror("msg_recv_ticket: msgrcv failed");
        }
        return ret;
    }
}

/* Receive ticket response from separate response queue */
ssize_t msg_recv_ticket_resp(ticket_msg_t *msg, long mtype, int flags) {
    ssize_t ret;
    while (1) {
        ret = msgrcv(g_msgid_ticket_resp, msg, sizeof(ticket_msg_t) - sizeof(long), mtype, flags);
        if (ret >= 0) {
            return ret;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != ENOMSG && errno != EIDRM) {
            perror("msg_recv_ticket_resp: msgrcv failed");
        }
        return ret;
    }
}

int msg_send_boarding(boarding_msg_t *msg) {
    while (1) {
        if (msgsnd(g_msgid_boarding, msg, sizeof(boarding_msg_t) - sizeof(long), 0) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != EIDRM && errno != EINVAL) {
            perror("msg_send_boarding: msgsnd failed");
        }
        return -1;
    }
}

/* Send boarding response to separate response queue - always has room */
int msg_send_boarding_resp(boarding_msg_t *msg) {
    while (1) {
        if (msgsnd(g_msgid_boarding_resp, msg, sizeof(boarding_msg_t) - sizeof(long), 0) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != EIDRM && errno != EINVAL) {
            perror("msg_send_boarding_resp: msgsnd failed");
        }
        return -1;
    }
}

ssize_t msg_recv_boarding(boarding_msg_t *msg, long mtype, int flags) {
    ssize_t ret;
    while (1) {
        ret = msgrcv(g_msgid_boarding, msg, sizeof(boarding_msg_t) - sizeof(long), mtype, flags);
        if (ret >= 0) {
            return ret;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != ENOMSG && errno != EIDRM && errno != EINVAL) {
            perror("msg_recv_boarding: msgrcv failed");
        }
        return ret;
    }
}

/* Receive boarding response from separate response queue */
ssize_t msg_recv_boarding_resp(boarding_msg_t *msg, long mtype, int flags) {
    ssize_t ret;
    while (1) {
        ret = msgrcv(g_msgid_boarding_resp, msg, sizeof(boarding_msg_t) - sizeof(long), mtype, flags);
        if (ret >= 0) {
            return ret;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != ENOMSG && errno != EIDRM && errno != EINVAL) {
            perror("msg_recv_boarding_resp: msgrcv failed");
        }
        return ret;
    }
}

int msg_send_dispatch(dispatch_msg_t *msg) {
    while (1) {
        if (msgsnd(g_msgid_dispatch, msg, sizeof(dispatch_msg_t) - sizeof(long), 0) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != EIDRM) {
            perror("msg_send_dispatch: msgsnd failed");
        }
        return -1;
    }
}

ssize_t msg_recv_dispatch(dispatch_msg_t *msg, long mtype, int flags) {
    ssize_t ret;
    while (1) {
        ret = msgrcv(g_msgid_dispatch, msg, sizeof(dispatch_msg_t) - sizeof(long), mtype, flags);
        if (ret >= 0) {
            return ret;
        }
        if (errno == EINTR) {
            /* Signal received (e.g., SIGTSTP/SIGCONT) - retry */
            continue;
        }
        if (errno != ENOMSG && errno != EIDRM) {
            perror("msg_recv_dispatch: msgrcv failed");
        }
        return ret;
    }
}

/* Safeguard: check message queue depths and warn if getting high */
void ipc_check_queue_health(void) {
    struct msqid_ds buf;
    
    /* Check ticket request queue */
    if (g_msgid_ticket != -1) {
        if (msgctl(g_msgid_ticket, IPC_STAT, &buf) == 0) {
            if ((int)buf.msg_qnum > MAX_TICKET_QUEUE_REQUESTS) {
                log_dispatcher(LOG_WARN, "Safeguard: Ticket queue depth high (%lu messages)", 
                              (unsigned long)buf.msg_qnum);
            }
        }
    }
    
    /* Check boarding request queue */
    if (g_msgid_boarding != -1) {
        if (msgctl(g_msgid_boarding, IPC_STAT, &buf) == 0) {
            if ((int)buf.msg_qnum > MAX_BOARDING_QUEUE_REQUESTS) {
                log_dispatcher(LOG_WARN, "Safeguard: Boarding queue depth high (%lu messages)", 
                              (unsigned long)buf.msg_qnum);
            }
        }
    }
}
