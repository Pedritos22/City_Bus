#include "common.h"
#include "ipc.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/msg.h>
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_early_departure = 0;
static int g_bus_id = 0;

static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
}

static void handle_early_departure(int sig) {
    (void)sig;
    g_early_departure = 1;
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGINT, &sa, NULL) == -1) perror("sigaction SIGINT");
    if (sigaction(SIGTERM, &sa, NULL) == -1) perror("sigaction SIGTERM");
    
    sa.sa_handler = handle_early_departure;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) perror("sigaction SIGUSR1");
}

static int can_board(shm_data_t *shm, const boarding_msg_t *request, char *reason) {
    bus_state_t *bus = &shm->buses[g_bus_id];
    const passenger_info_t *p = &request->passenger;
    
    /* Check if passenger has valid ticket */
    if (!p->has_ticket && !p->is_vip) {
        snprintf(reason, 64, "No valid ticket");
        return 0;
    }
    
    /* Check if boarding is allowed */
    if (!shm->boarding_allowed) {
        snprintf(reason, 64, "Boarding blocked by dispatcher");
        return 0;
    }
    
    /* Check if bus is at station */
    if (!bus->at_station) {
        snprintf(reason, 64, "Bus not at station");
        return 0;
    }
    
    /* Check if boarding is open for this bus */
    if (!bus->boarding_open) {
        snprintf(reason, 64, "Bus boarding not open");
        return 0;
    }
    
    /* Check passenger capacity - need room for seat_count seats
     * (1 for adult alone, 2 for adult with child) */
    int seats_needed = p->seat_count > 0 ? p->seat_count : 1;
    if (bus->passenger_count + seats_needed > BUS_CAPACITY) {
        snprintf(reason, 64, "Not enough seats (%d needed, %d available)", 
                 seats_needed, BUS_CAPACITY - bus->passenger_count);
        return 0;
    }
    
    /* Check bicycle capacity if passenger has bike */
    if (p->has_bike && bus->bike_count >= BIKE_CAPACITY) {
        snprintf(reason, 64, "Bus at bicycle capacity (%d/%d)", 
                 bus->bike_count, BIKE_CAPACITY);
        return 0;
    }
    
    
    reason[0] = '\0';
    return 1;
}

/* Validate boarding request message */
static int validate_boarding_request(const boarding_msg_t *request) {
    /* Check mtype is valid boarding request */
    if (request->mtype != MSG_BOARD_REQUEST && request->mtype != MSG_BOARD_REQUEST_VIP) {
        log_driver(LOG_ERROR, "Bus %d: Invalid message type %ld", g_bus_id, request->mtype);
        return 0;
    }
    
    /* Check passenger PID is valid */
    if (request->passenger.pid <= 0) {
        log_driver(LOG_ERROR, "Bus %d: Invalid passenger PID %d", g_bus_id, request->passenger.pid);
        return 0;
    }
    
    /* Check seat count is reasonable */
    if (request->passenger.seat_count <= 0 || request->passenger.seat_count > 2) {
        log_driver(LOG_ERROR, "Bus %d: Invalid seat count %d from PID %d", 
                  g_bus_id, request->passenger.seat_count, request->passenger.pid);
        return 0;
    }
    
    /* Check age is valid */
    if (request->passenger.age < MIN_AGE || request->passenger.age > MAX_AGE) {
        log_driver(LOG_ERROR, "Bus %d: Invalid age %d from PID %d",
                  g_bus_id, request->passenger.age, request->passenger.pid);
        return 0;
    }
    
    return 1;
}

static void process_boarding_request(shm_data_t *shm, boarding_msg_t *request) {
    boarding_msg_t response;
    memset(&response, 0, sizeof(response));
    
    /* Set response mtype to passenger's PID */
    response.mtype = request->passenger.pid;
    response.passenger = request->passenger;
    response.bus_id = g_bus_id;
    
    int seats = request->passenger.seat_count > 0 ? request->passenger.seat_count : 1;
    sem_lock(SEM_SHM_MUTEX);
    if (can_board(shm, request, response.reason)) {
        response.approved = true;
        
        shm->buses[g_bus_id].entering_count++;
        sem_unlock(SEM_SHM_MUTEX);
        int entrance_sem = request->passenger.has_bike ? 
                          SEM_ENTRANCE_BIKE : SEM_ENTRANCE_PASSENGER;
        sem_lock(entrance_sem);
        
        if (!log_is_perf_mode()) {
            usleep(seats * 300000);
        }
        sem_lock(SEM_SHM_MUTEX);
        shm->buses[g_bus_id].passenger_count += seats;  /* Count all seats */
        if (request->passenger.has_bike) {
            shm->buses[g_bus_id].bike_count++;
        }
        shm->buses[g_bus_id].entering_count--;
        shm->passengers_waiting -= seats;
        if (shm->passengers_waiting < 0) {
            shm->passengers_waiting = 0;
        }
        shm->boarded_people += seats;
        if (request->passenger.is_vip) {
            shm->boarded_vip_people += seats;
        }
        int current_count = shm->buses[g_bus_id].passenger_count;
        int current_bikes = shm->buses[g_bus_id].bike_count;
        sem_unlock(SEM_SHM_MUTEX);
        
        /* Release entrance */
        sem_unlock(entrance_sem);
        if (request->passenger.is_vip) {
            log_driver(LOG_INFO, "Bus %d: VIP PID %d priority boarded (Total: %d/%d)",
                      g_bus_id, request->passenger.pid, current_count, BUS_CAPACITY);
        } else if (request->passenger.has_child_with) {
            log_driver(LOG_INFO, "Bus %d: Adult PID %d + child boarded (%d seats) (Total: %d/%d, Bikes: %d/%d)",
                      g_bus_id, request->passenger.pid, seats,
                      current_count, BUS_CAPACITY, current_bikes, BIKE_CAPACITY);
        } else {
            log_driver(LOG_INFO, "Bus %d: Passenger PID %d boarded (Total: %d/%d, Bikes: %d/%d)",
                      g_bus_id, request->passenger.pid,
                      current_count, BUS_CAPACITY, current_bikes, BIKE_CAPACITY);
        }
    } else {
        response.approved = false;
        sem_unlock(SEM_SHM_MUTEX);
        
        log_driver(LOG_WARN, "Bus %d: Boarding denied for PID %d - %s",
                  g_bus_id, request->passenger.pid, response.reason);
    }
    if (msg_send_boarding_resp(&response) == -1) {
        log_driver(LOG_ERROR, "Bus %d: Failed to send boarding response to PID %d",
                  g_bus_id, request->passenger.pid);
    }
}

static void wait_for_entrance_clear(shm_data_t *shm) {
    int entering;
    do {
        sem_lock(SEM_SHM_MUTEX);
        entering = shm->buses[g_bus_id].entering_count;
        sem_unlock(SEM_SHM_MUTEX);
        
        if (entering > 0) {
            log_driver(LOG_INFO, "Bus %d: Waiting for %d passengers to finish entering",
                      g_bus_id, entering);
            usleep(100000);
        }
    } while (entering > 0 && g_running);
}

static void depart_bus(shm_data_t *shm) {
    bus_state_t *bus = &shm->buses[g_bus_id];
    wait_for_entrance_clear(shm);
    
    sem_lock(SEM_SHM_MUTEX);
    bus->boarding_open = false;
    bus->at_station = false;
    int return_delay = MIN_RETURN_TIME + rand() % (MAX_RETURN_TIME - MIN_RETURN_TIME + 1);
    bus->return_time = time(NULL) + return_delay;
    
    int passengers = bus->passenger_count;
    int bikes = bus->bike_count;
    shm->passengers_transported += passengers;
    int transported_after = shm->passengers_transported;
    
    sem_unlock(SEM_SHM_MUTEX);
    
    log_driver(LOG_INFO, "Bus %d: DEPARTED with %d passengers and %d bikes (return in %d seconds) - transported count now: %d",
              g_bus_id, passengers, bikes, return_delay, transported_after);
    if (!log_is_perf_mode()) {
        sleep(return_delay);
    }
    else {
        usleep(10000);
    }
    sem_lock(SEM_SHM_MUTEX);
    bus->at_station = true;
    bus->passenger_count = 0;
    bus->bike_count = 0;
    bus->boarding_open = true;
    int boarding_interval = log_is_perf_mode() ? 1 : BOARDING_INTERVAL;
    bus->departure_time = time(NULL) + boarding_interval;
    int current_active = shm->active_bus_id;
    if (current_active < 0 || !shm->buses[current_active].at_station) {
        shm->active_bus_id = g_bus_id;
        log_driver(LOG_INFO, "Bus %d: Became active bus (previous active %d not at station)", 
                  g_bus_id, current_active);
    }
    sem_unlock(SEM_SHM_MUTEX);
    
    log_driver(LOG_INFO, "Bus %d: RETURNED to station, boarding open",
              g_bus_id);
}

static int check_shutdown(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int running = shm->simulation_running;
    int waiting = shm->passengers_waiting;
    int station_closed = shm->station_closed;
    sem_unlock(SEM_SHM_MUTEX);
    
    /* Don't shutdown if passengers are still waiting (even if station closed) */
    if (station_closed && waiting > 0) {
        return 0;  // Keep working until all passengers transported
    }
    
    return !running;
}

static int g_depart_when_full = 0;  /* Set by --full flag */

static int should_depart(shm_data_t *shm) {
    time_t now = time(NULL);
    
    sem_lock(SEM_SHM_MUTEX);
    time_t depart_time = shm->buses[g_bus_id].departure_time;
    int passengers = shm->buses[g_bus_id].passenger_count;
    int at_capacity = (passengers >= BUS_CAPACITY);
    sem_unlock(SEM_SHM_MUTEX);
    
    /* Optional: depart immediately when full (--full flag) */
    if (g_depart_when_full && at_capacity) {
        log_driver(LOG_INFO, "Bus %d: Departing - at full capacity (%d passengers)", g_bus_id, passengers);
        return 1;
    }
    
    /* Depart when scheduled time reached AND have passengers */
    if (depart_time > 0 && now >= depart_time && passengers > 0) {
        log_driver(LOG_INFO, "Bus %d: Departing - scheduled time reached (passengers: %d)", g_bus_id, passengers);
        return 1;
    }
    
    /* Debug: log why not departing (only occasionally to avoid spam) */
    static int debug_counter = 0;
    if (++debug_counter % 500 == 0 && passengers > 0 && depart_time > 0) {
        log_driver(LOG_INFO, "Bus %d: waiting - now=%ld, depart_time=%ld, diff=%ld, passengers=%d",
                   g_bus_id, (long)now, (long)depart_time, (long)(depart_time - now), passengers);
    }
    
    /* Early departure signal (SIGUSR1) with passengers */
    if (g_early_departure && passengers > 0) {
        log_driver(LOG_INFO, "Bus %d: Departing early with %d passengers (SIGUSR1)", g_bus_id, passengers);
        g_early_departure = 0;
        return 1;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        g_bus_id = atoi(argv[1]);
    }
    
    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    /* Check for --full flag (depart when full) */
    const char *full_depart = getenv("BUS_FULL_DEPART");
    if (full_depart && (strcmp(full_depart, "1") == 0 || strcasecmp(full_depart, "true") == 0)) {
        g_depart_when_full = 1;
    }
    
    if (!is_minimal) {
        printf("[DRIVER %d] Starting (PID=%d)\n", g_bus_id, getpid());
        fflush(stdout);
    }
    
    /* Seed random number generator */
    srand(time(NULL) ^ getpid());
    
    setup_signals();
    if (ipc_attach_all() != 0) {
        fprintf(stderr, "[DRIVER %d] Failed to attach to IPC resources\n", g_bus_id);
        exit(EXIT_FAILURE);
    }
    
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "[DRIVER %d] Failed to get shared memory\n", g_bus_id);
        exit(EXIT_FAILURE);
    }
    
    sem_lock(SEM_SHM_MUTEX);
    shm->driver_pids[g_bus_id] = getpid();
    shm->buses[g_bus_id].at_station = true;
    shm->buses[g_bus_id].boarding_open = true;
    shm->buses[g_bus_id].passenger_count = 0;
    shm->buses[g_bus_id].bike_count = 0;
    shm->buses[g_bus_id].entering_count = 0;
    int boarding_interval = log_is_perf_mode() ? 1 : BOARDING_INTERVAL;
    shm->buses[g_bus_id].departure_time = time(NULL) + boarding_interval;
    
    if (g_bus_id == 0) {
        shm->active_bus_id = 0;
    }
    sem_unlock(SEM_SHM_MUTEX);
    log_driver(LOG_INFO, "Bus %d driver started (PID=%d)", g_bus_id, getpid());
    
    int was_active = (g_bus_id == 0);  /* Only bus 0 starts active */
    
    while (g_running) {
        if (check_shutdown(shm)) {
            log_driver(LOG_INFO, "Bus %d: Shutdown detected", g_bus_id);
            break;
        }
        sem_lock(SEM_SHM_MUTEX);
        int at_station = shm->buses[g_bus_id].at_station;
        int boarding_open = shm->buses[g_bus_id].boarding_open;
        int am_active = (shm->active_bus_id == g_bus_id);
        
        /* Just became active, reset departure time */
        if (am_active && !was_active && at_station) {
            int boarding_interval = log_is_perf_mode() ? 1 : BOARDING_INTERVAL;
            shm->buses[g_bus_id].departure_time = time(NULL) + boarding_interval;
            log_driver(LOG_INFO, "Bus %d: Became active, departure in %d sec", 
                      g_bus_id, boarding_interval);
        }
        was_active = am_active;
        sem_unlock(SEM_SHM_MUTEX);
        
        /* Only the active bus receives passengers; others wait */
        if (!at_station || !boarding_open || !am_active) {
            usleep(log_is_perf_mode() ? 10000 : 100000);
            continue;
        }
        if (log_is_perf_mode() && should_depart(shm)) {
            sem_lock(SEM_SHM_MUTEX);
            int next_bus = -1;
            for (int i = 0; i < MAX_BUSES; i++) {
                int check_bus = (g_bus_id + 1 + i) % MAX_BUSES;
                if (shm->buses[check_bus].at_station && check_bus != g_bus_id) {
                    next_bus = check_bus;
                    break;
                }
            }
            
            /* Only switch if we found a bus at station */
            if (next_bus >= 0) {
                shm->active_bus_id = next_bus;
                sem_unlock(SEM_SHM_MUTEX);
                log_driver(LOG_INFO, "Bus %d: Switching active bus to %d", g_bus_id, next_bus);
            } else {
                /* No other bus at station - set to -1 (will be set when next bus returns) */
                shm->active_bus_id = -1;
                sem_unlock(SEM_SHM_MUTEX);
                log_driver(LOG_INFO, "Bus %d: No other bus at station, active_bus_id set to -1", g_bus_id);
            }
            
            /* Depart */
            depart_bus(shm);
            continue;
        }
        /* Receive boarding request - negative mtype receives lowest type first (VIP=1 before regular=2) */
        boarding_msg_t request;
        ssize_t ret = msg_recv_boarding(&request, -MSG_BOARD_REQUEST, 0);
        if (ret > 0) {
            /* Validate message before processing */
            if (!validate_boarding_request(&request)) {
                log_driver(LOG_WARN, "Bus %d: Discarding invalid boarding request", g_bus_id);
                continue;
            }
            process_boarding_request(shm, &request);
            /* Passenger unlocks SEM_BOARDING_QUEUE_SLOTS after receiving response */
            if (log_is_perf_mode() && should_depart(shm)) {
                sem_lock(SEM_SHM_MUTEX);
                int next_bus = -1;
                
                /* Find next available bus at station */
                for (int i = 0; i < MAX_BUSES; i++) {
                    int check_bus = (g_bus_id + 1 + i) % MAX_BUSES;
                    if (shm->buses[check_bus].at_station && check_bus != g_bus_id) {
                        next_bus = check_bus;
                        break;
                    }
                }
                
                /* Only switch if we found a bus at station */
                if (next_bus >= 0) {
                    shm->active_bus_id = next_bus;
                    sem_unlock(SEM_SHM_MUTEX);
                    log_driver(LOG_INFO, "Bus %d: Switching active bus to %d", g_bus_id, next_bus);
                } else {
                    /* No other bus at station - set to -1 (will be set when next bus returns) */
                    shm->active_bus_id = -1;
                    sem_unlock(SEM_SHM_MUTEX);
                    log_driver(LOG_INFO, "Bus %d: No other bus at station, active_bus_id set to -1", g_bus_id);
                }
                
                /* Depart */
                depart_bus(shm);
                continue;
            }
        }
        if (should_depart(shm)) {
            sem_lock(SEM_SHM_MUTEX);
            int next_bus = -1;
            
            /* Find next available bus at station */
            for (int i = 0; i < MAX_BUSES; i++) {
                int check_bus = (g_bus_id + 1 + i) % MAX_BUSES;
                if (shm->buses[check_bus].at_station && check_bus != g_bus_id) {
                    next_bus = check_bus;
                    break;
                }
            }
            
            /* Only switch if we found a bus at station */
            if (next_bus >= 0) {
                shm->active_bus_id = next_bus;
                sem_unlock(SEM_SHM_MUTEX);
                log_driver(LOG_INFO, "Bus %d: Switching active bus to %d", g_bus_id, next_bus);
            } else {
                /* No other bus at station - set to -1 (will be set when next bus returns) */
                shm->active_bus_id = -1;
                sem_unlock(SEM_SHM_MUTEX);
                log_driver(LOG_INFO, "Bus %d: No other bus at station, active_bus_id set to -1", g_bus_id);
            }
            
            depart_bus(shm);
        }
    }
    log_driver(LOG_INFO, "Bus %d driver shutting down", g_bus_id);
    
    sem_lock(SEM_SHM_MUTEX);
    shm->driver_pids[g_bus_id] = 0;
    shm->buses[g_bus_id].boarding_open = false;
    sem_unlock(SEM_SHM_MUTEX);
    
    ipc_detach_all();
    if (!is_minimal) {
        printf("[DRIVER %d] Terminated\n", g_bus_id);
    }
    return 0;
}
