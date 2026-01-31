#include "ipc.h"
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
static int g_msgid_boarding = -1;
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
        return -1;
    }

    memset(g_shm, 0, sizeof(shm_data_t));
    g_semid = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0600);
    if (g_semid == -1) {
        perror("ipc_create_all: semget failed");
        return -1;
    }

    union semun arg;
    arg.val = 1;
    if (semctl(g_semid, SEM_SHM_MUTEX, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_SHM_MUTEX failed");
        return -1;
    }
    if (semctl(g_semid, SEM_LOG_MUTEX, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_LOG_MUTEX failed");
        return -1;
    }
    if (semctl(g_semid, SEM_STATION_ENTRY, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_STATION_ENTRY failed");
        return -1;
    }
    if (semctl(g_semid, SEM_ENTRANCE_PASSENGER, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_ENTRANCE_PASSENGER failed");
        return -1;
    }
    if (semctl(g_semid, SEM_ENTRANCE_BIKE, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_ENTRANCE_BIKE failed");
        return -1;
    }
    if (semctl(g_semid, SEM_BOARDING_MUTEX, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_BOARDING_MUTEX failed");
        return -1;
    }
    
    arg.val = 0;
    if (semctl(g_semid, SEM_BUS_READY, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_BUS_READY failed");
        return -1;
    }
    
    arg.val = 1;
    for (int i = 0; i < TICKET_OFFICES; i++) {
        int sem_idx = SEM_TICKET_OFFICE(i);
        if (semctl(g_semid, sem_idx, SETVAL, arg) == -1) {
            fprintf(stderr, "ipc_create_all: semctl SEM_TICKET_OFFICE_%d failed\n", i);
            perror("semctl");
            return -1;
        }
    }

    arg.val = MAX_TICKET_QUEUE_REQUESTS;
    if (semctl(g_semid, SEM_TICKET_QUEUE_SLOTS, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_TICKET_QUEUE_SLOTS failed");
        return -1;
    }
    arg.val = MAX_BOARDING_QUEUE_REQUESTS;
    if (semctl(g_semid, SEM_BOARDING_QUEUE_SLOTS, SETVAL, arg) == -1) {
        perror("ipc_create_all: semctl SEM_BOARDING_QUEUE_SLOTS failed");
        return -1;
    }

    g_msgid_ticket = msgget(MSG_TICKET_KEY, IPC_CREAT | 0600);
    if (g_msgid_ticket == -1) {
        perror("ipc_create_all: msgget ticket failed");
        return -1;
    }

    g_msgid_boarding = msgget(MSG_BOARDING_KEY, IPC_CREAT | 0600);
    if (g_msgid_boarding == -1) {
        perror("ipc_create_all: msgget boarding failed");
        return -1;
    }

    g_msgid_dispatch = msgget(MSG_DISPATCH_KEY, IPC_CREAT | 0600);
    if (g_msgid_dispatch == -1) {
        perror("ipc_create_all: msgget dispatch failed");
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

    g_msgid_boarding = msgget(MSG_BOARDING_KEY, 0600);
    if (g_msgid_boarding == -1) {
        perror("ipc_attach_all: msgget boarding failed");
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
    if (g_shmid != -1) {
        if (shmctl(g_shmid, IPC_RMID, NULL) == -1) {
            if (errno != EINVAL && errno != EIDRM) {
                perror("ipc_cleanup_all: shmctl IPC_RMID failed");
            }
        }
        g_shmid = -1;
    }

    if (g_semid != -1) {
        if (semctl(g_semid, 0, IPC_RMID) == -1) {
            if (errno != EINVAL && errno != EIDRM) {
                perror("ipc_cleanup_all: semctl IPC_RMID failed");
            }
        }
        g_semid = -1;
    }

    if (g_msgid_ticket != -1) {
        if (msgctl(g_msgid_ticket, IPC_RMID, NULL) == -1) {
            if (errno != EINVAL && errno != EIDRM) {
                perror("ipc_cleanup_all: msgctl ticket IPC_RMID failed");
            }
        }
        g_msgid_ticket = -1;
    }

    if (g_msgid_boarding != -1) {
        if (msgctl(g_msgid_boarding, IPC_RMID, NULL) == -1) {
            if (errno != EINVAL && errno != EIDRM) {
                perror("ipc_cleanup_all: msgctl boarding IPC_RMID failed");
            }
        }
        g_msgid_boarding = -1;
    }

    if (g_msgid_dispatch != -1) {
        if (msgctl(g_msgid_dispatch, IPC_RMID, NULL) == -1) {
            if (errno != EINVAL && errno != EIDRM) {
                perror("ipc_cleanup_all: msgctl dispatch IPC_RMID failed");
            }
        }
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
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = 0;

    if (semop(g_semid, &op, 1) == -1) {
        if (errno == EINTR || errno == EIDRM) {
            return -1;
        }
        perror("sem_lock: semop failed");
        exit(EXIT_FAILURE);
    }
    return 0;
}

void sem_unlock(int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;
    op.sem_flg = 0;

    if (semop(g_semid, &op, 1) == -1) {
        if (errno != EINTR && errno != EIDRM) {
            perror("sem_unlock: semop failed");
            exit(EXIT_FAILURE);
        }
    }
}

int sem_getval(int sem_num) {
    int val = semctl(g_semid, sem_num, GETVAL);
    if (val == -1) {
        if (errno != EIDRM) {
            perror("sem_getval: semctl failed");
        }
        return 0;
    }
    return val;
}

void sem_setval(int sem_num, int value) {
    union semun arg;
    arg.val = value;
    
    if (semctl(g_semid, sem_num, SETVAL, arg) == -1) {
        perror("sem_setval: semctl failed");
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

int msg_send_ticket(ticket_msg_t *msg) {
    if (msgsnd(g_msgid_ticket, msg, sizeof(ticket_msg_t) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM) {
            perror("msg_send_ticket: msgsnd failed");
        }
        return -1;
    }
    return 0;
}

ssize_t msg_recv_ticket(ticket_msg_t *msg, long mtype, int flags) {
    ssize_t ret = msgrcv(g_msgid_ticket, msg, sizeof(ticket_msg_t) - sizeof(long), mtype, flags);
    if (ret == -1 && errno != ENOMSG && errno != EINTR && errno != EIDRM) {
        perror("msg_recv_ticket: msgrcv failed");
    }
    return ret;
}

int msg_send_boarding(boarding_msg_t *msg) {
    if (msgsnd(g_msgid_boarding, msg, sizeof(boarding_msg_t) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM) {
            perror("msg_send_boarding: msgsnd failed");
        }
        return -1;
    }
    return 0;
}

ssize_t msg_recv_boarding(boarding_msg_t *msg, long mtype, int flags) {
    ssize_t ret = msgrcv(g_msgid_boarding, msg, sizeof(boarding_msg_t) - sizeof(long), mtype, flags);
    if (ret == -1 && errno != ENOMSG && errno != EINTR && errno != EIDRM) {
        perror("msg_recv_boarding: msgrcv failed");
    }
    return ret;
}

int msg_send_dispatch(dispatch_msg_t *msg) {
    if (msgsnd(g_msgid_dispatch, msg, sizeof(dispatch_msg_t) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM) {
            perror("msg_send_dispatch: msgsnd failed");
        }
        return -1;
    }
    return 0;
}

ssize_t msg_recv_dispatch(dispatch_msg_t *msg, long mtype, int flags) {
    ssize_t ret = msgrcv(g_msgid_dispatch, msg, sizeof(dispatch_msg_t) - sizeof(long), mtype, flags);
    if (ret == -1 && errno != ENOMSG && errno != EINTR && errno != EIDRM) {
        perror("msg_recv_dispatch: msgrcv failed");
    }
    return ret;
}
