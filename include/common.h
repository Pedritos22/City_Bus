#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <stdbool.h>
#include "config.h"

enum SemaphoreIndex {
    SEM_SHM_MUTEX = 0,
    SEM_LOG_MUTEX,
    SEM_STATION_ENTRY,
    SEM_ENTRANCE_PASSENGER,
    SEM_ENTRANCE_BIKE,
    SEM_BOARDING_MUTEX,
    SEM_BUS_READY,
    SEM_TICKET_OFFICE_0,
    SEM_TICKET_OFFICE_1,
    SEM_COUNT
};

enum TicketMsgType {
    MSG_TICKET_REQUEST = 1,
    MSG_TICKET_GRANTED = 2
};

enum BoardingMsgType {
    MSG_BOARD_REQUEST = 1,
    MSG_BOARD_GRANTED = 2,
    MSG_BOARD_DENIED = 3,
    MSG_BOARD_WAIT = 4
};

enum DispatchMsgType {
    MSG_DISPATCH_DEPART = 1,
    MSG_DISPATCH_BLOCK = 2,
    MSG_DISPATCH_UNBLOCK = 3,
    MSG_DISPATCH_SHUTDOWN = 99
};

typedef struct {
    int id;
    bool at_station;
    bool boarding_open;
    int passenger_count;
    int bike_count;
    int entering_count;
    time_t departure_time;
    time_t return_time;
} bus_state_t;

typedef struct {
    bool simulation_running;
    bool station_open;
    bool boarding_allowed;
    bool early_departure_flag;

    int total_passengers_created;
    int passengers_transported;
    int passengers_waiting;
    int passengers_in_office;

    bus_state_t buses[MAX_BUSES];
    int active_bus_id;

    int ticket_office_busy[TICKET_OFFICES];
    int tickets_issued;

    pid_t dispatcher_pid;
    pid_t driver_pids[MAX_BUSES];
    pid_t ticket_office_pids[TICKET_OFFICES];
} shm_data_t;

typedef struct {
    pid_t pid;
    int age;
    bool has_bike;
    bool is_vip;
    bool has_ticket;
    bool is_child;
    bool has_child_with;
    int child_age;
    int seat_count;
    int assigned_bus;
} passenger_info_t;

typedef struct {
    long mtype;
    passenger_info_t passenger;
    int ticket_office_id;
    bool approved;
} ticket_msg_t;

typedef struct {
    long mtype;
    passenger_info_t passenger;
    int bus_id;
    bool approved;
    char reason[64];
} boarding_msg_t;

typedef struct {
    long mtype;
    pid_t sender_pid;
    int target_bus;
    char details[64];
} dispatch_msg_t;

#define IS_CHILD(age) ((age) < CHILD_AGE_LIMIT)
#define BUS_HAS_PASSENGER_SPACE(bus) ((bus).passenger_count < BUS_CAPACITY)
#define BUS_HAS_BIKE_SPACE(bus) ((bus).bike_count < BIKE_CAPACITY)
#define BUS_ENTRANCE_CLEAR(bus) ((bus).entering_count == 0)

#endif
