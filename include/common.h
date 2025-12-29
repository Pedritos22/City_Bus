#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include "config.h"

// IPC keys
#define SHM_KEY 0x2211
#define SEM_KEY 0x2834
#define MSG_KEY 0x4321

// Semaphores
enum {
    SEM_REGISTER = 0,      // Ticket office access
    SEM_LOG,               // Logging access
    SEM_STATION,           // Boarding/entrance access
    SEM_ENTRANCE_PASS,     // Passenger entrance
    SEM_ENTRANCE_BIKE,     // Bicycle entrance
    SEM_COUNT
};

// Passenger message
typedef struct {
    long mtype;
    pid_t pid;
    int has_bike;
    int is_vip;
    int age;
    int ticket_issued; // ticket confirmation
    pid_t adult_pid;   // for children
} passenger_msg_t;

// Shared memory structure
typedef struct {
    int station_open;
    int boarding_allowed;
    int waiting_passengers;

    int bus_present[MAX_BUSES];
    int bus_passengers[MAX_BUSES];
    int bus_bikes[MAX_BUSES];
} shm_data_t;

#endif
