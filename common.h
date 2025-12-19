#ifndef COMMON_H
#define COMMON_H

#define SHM_KEY 0x2211
#define SEM_KEY 0x2834

#define N 3
#define BUS_SEATS 10
#define BIKES 4

#define SEM_REGISTER 0
#define SEM_LOG 1
#define SEM_STATION 2
#define SEM_GLOBAL 3
#define SEM_COUNT 4

typedef struct {
    int station_open; // 1 open, 0 closed, to check if a passenger can even come
    int waiting_passengers;
    int bus_present[N]; // bus availability 1/0
    int bus_passengers[N]; // how many passengers in a bus
    int bus_bikes[N]; // how many bikes in a bus
} shm_data_t;

#endif
