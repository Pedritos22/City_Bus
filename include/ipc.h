#ifndef IPC_H
#define IPC_H

#include "common.h"

// IPC functions
int create_ipc(void);
void cleanup_ipc(void);
void sem_lock(int sem_id, int sem_num);
void sem_unlock(int sem_id, int sem_num);

#endif
