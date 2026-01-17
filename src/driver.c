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
static int g_bus_id = 0;  //  This driver bus ID


/**
 * Handler for SIGTERM/SIGINT - shutdown signal.
 */
static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
}

/**
 * Handler for SIGUSR1 - early departure (forwarded from dispatcher).
 */
static void handle_early_departure(int sig) {
    (void)sig;
    g_early_departure = 1;
}

/**
 * Setup signal handlers.
 */
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


/**
 * Check if boarding request can be approved.
 * Validates ticket, capacity, and handles adults with children.
 * 
 * Children are handled as threads within the adult's process:
 * - An adult with a child has seat_count = 2
 * - We check if there's room for all seats needed
 * - The child is guaranteed to be supervised (same process)
 */
static int can_board(shm_data_t *shm, const boarding_msg_t *request, char *reason) {
    bus_state_t *bus = &shm->buses[g_bus_id];
    const passenger_info_t *p = &request->passenger;
    
    // Check if passenger has valid ticket
    if (!p->has_ticket && !p->is_vip) {
        snprintf(reason, 64, "No valid ticket");
        return 0;
    }
    
    // Check if boarding is allowed
    if (!shm->boarding_allowed) {
        snprintf(reason, 64, "Boarding blocked by dispatcher");
        return 0;
    }
    
    // Check if bus is at station
    if (!bus->at_station) {
        snprintf(reason, 64, "Bus not at station");
        return 0;
    }
    
    // Check if boarding is open for this busw
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
    
    // Check bicycle capacity if passenger has bike
    if (p->has_bike && bus->bike_count >= BIKE_CAPACITY) {
        snprintf(reason, 64, "Bus at bicycle capacity (%d/%d)", 
                 bus->bike_count, BIKE_CAPACITY);
        return 0;
    }
    
    /* Note: Child supervision is guaranteed because children are threads
     * within the adult's process - they can't board separately */
    
    reason[0] = '\0';
    return 1;
}

/**
 * Process a single boarding request.
 * 
 * Handles adults with children:
 * - seat_count indicates how many seats are needed (1 or 2)
 * - Both adult and child board together (child is a thread)
 */
static void process_boarding_request(shm_data_t *shm, boarding_msg_t *request) {
    boarding_msg_t response;
    memset(&response, 0, sizeof(response));
    
    // Set response mtype to passenger PID
    response.mtype = request->passenger.pid;
    response.passenger = request->passenger;
    response.bus_id = g_bus_id;
    
    // Get seat count (1 for adult alone, 2 for adult with child)
    int seats = request->passenger.seat_count > 0 ? request->passenger.seat_count : 1;
    
    sem_lock(SEM_SHM_MUTEX);
    
    // Check if boarding is possible
    if (can_board(shm, request, response.reason)) {
        response.approved = true;
        
        // Mark passenger as entering
        shm->buses[g_bus_id].entering_count++;
        
        sem_unlock(SEM_SHM_MUTEX);
        
        // Lock the appropriate entrance (one at a time)
        int entrance_sem = request->passenger.has_bike ? 
                          SEM_ENTRANCE_BIKE : SEM_ENTRANCE_PASSENGER;
        sem_lock(entrance_sem);
        
        // Simulate boarding time (longer if with child) TODO: ASK IF OKAY
        
        // Update bus state after boarding
        sem_lock(SEM_SHM_MUTEX);
        shm->buses[g_bus_id].passenger_count += seats;  // Count all seats
        if (request->passenger.has_bike) {
            shm->buses[g_bus_id].bike_count++;
        }
        shm->buses[g_bus_id].entering_count--;
        shm->passengers_waiting -= seats;  // Remove all from waiting
        if (shm->passengers_waiting < 0) {
            shm->passengers_waiting = 0;
        }
        sem_unlock(SEM_SHM_MUTEX);
        
        // Release entrance
        sem_unlock(entrance_sem);
        
        if (request->passenger.has_child_with) {
            log_driver(LOG_INFO, "Bus %d: Adult PID %d + child boarded (%d seats) (Total: %d/%d, Bikes: %d/%d)",
                      g_bus_id, request->passenger.pid, seats,
                      shm->buses[g_bus_id].passenger_count, BUS_CAPACITY,
                      shm->buses[g_bus_id].bike_count, BIKE_CAPACITY);
        } else {
            log_driver(LOG_INFO, "Bus %d: Passenger PID %d boarded (Total: %d/%d, Bikes: %d/%d)",
                      g_bus_id, request->passenger.pid,
                      shm->buses[g_bus_id].passenger_count, BUS_CAPACITY,
                      shm->buses[g_bus_id].bike_count, BIKE_CAPACITY);
        }
    } else {
        response.approved = false;
        sem_unlock(SEM_SHM_MUTEX);
        
        log_driver(LOG_WARN, "Bus %d: Boarding denied for PID %d - %s",
                  g_bus_id, request->passenger.pid, response.reason);
    }
    
    // Send response to passenger
    if (msg_send_boarding(&response) == -1) {
        log_driver(LOG_ERROR, "Bus %d: Failed to send boarding response to PID %d",
                  g_bus_id, request->passenger.pid);
    }
}

/**
 * Wait for all passengers to finish entering the bus.
 */
static void wait_for_entrance_clear(shm_data_t *shm) {
    int entering;
    do {
        sem_lock(SEM_SHM_MUTEX);
        entering = shm->buses[g_bus_id].entering_count;
        sem_unlock(SEM_SHM_MUTEX);
        
        if (entering > 0) {
            log_driver(LOG_INFO, "Bus %d: Waiting for %d passengers to finish entering",
                      g_bus_id, entering);
            // 100ms wait TOOD: ASK IF OKAY
            usleep(100000); 
        }
    } while (entering > 0 && g_running);
}

// Departing the bus
static void depart_bus(shm_data_t *shm) {
    bus_state_t *bus = &shm->buses[g_bus_id];
    
    // Wait for entrance to be clear
    wait_for_entrance_clear(shm);
    
    sem_lock(SEM_SHM_MUTEX);
    
    // Close boarding 
    bus->boarding_open = false;
    
    // Mark as not at station
    bus->at_station = false;
    
    // Set return time
    int return_delay = MIN_RETURN_TIME + rand() % (MAX_RETURN_TIME - MIN_RETURN_TIME + 1);
    bus->return_time = time(NULL) + return_delay;
    
    int passengers = bus->passenger_count;
    int bikes = bus->bike_count;
    
    // Update transported count
    shm->passengers_transported += passengers;
    
    // Clear early departure flag if set
    shm->early_departure_flag = false;
    
    sem_unlock(SEM_SHM_MUTEX);
    
    log_driver(LOG_INFO, "Bus %d: DEPARTED with %d passengers and %d bikes (return in %d seconds)",
              g_bus_id, passengers, bikes, return_delay);
    
    // Simulate trip TODO: ASK IF OKAY, I HOPE I DO NOT NEED TO ADD A WHILE OR SOMETHING
    sleep(return_delay);
    
    // Return to station
    sem_lock(SEM_SHM_MUTEX);
    bus->at_station = true;
    bus->passenger_count = 0;
    bus->bike_count = 0;
    bus->boarding_open = true;
    bus->departure_time = time(NULL) + BOARDING_INTERVAL;
    sem_unlock(SEM_SHM_MUTEX);
    
    log_driver(LOG_INFO, "Bus %d: RETURNED to station, boarding open",
              g_bus_id);
}

/**
 * Check for shutdown via shared memory or dispatcher message!!!
 */
static int check_shutdown(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int running = shm->simulation_running;
    sem_unlock(SEM_SHM_MUTEX);
    
    return !running;
}

/**
 * Check for dispatcher commands (non-blocking).
 */
static void check_dispatcher_commands(shm_data_t *shm) {
    dispatch_msg_t msg;
    
    // Check for early departure command
    if (msg_recv_dispatch(&msg, MSG_DISPATCH_DEPART, IPC_NOWAIT) > 0) {
        log_driver(LOG_INFO, "Bus %d: Received early departure command", g_bus_id);
        g_early_departure = 1;
    }
    
    // Check for shutdown command
    if (msg_recv_dispatch(&msg, MSG_DISPATCH_SHUTDOWN, IPC_NOWAIT) > 0) {
        log_driver(LOG_INFO, "Bus %d: Received shutdown command", g_bus_id);
        g_running = 0;
    }
    
    // Check shared memory for early departure flag
    sem_lock(SEM_SHM_MUTEX);
    if (shm->early_departure_flag) {
        g_early_departure = 1;
    }
    sem_unlock(SEM_SHM_MUTEX);
}

/**
 * Check if it's time to depart.
 */
static int should_depart(shm_data_t *shm) {
    time_t now = time(NULL);
    
    sem_lock(SEM_SHM_MUTEX);
    time_t depart_time = shm->buses[g_bus_id].departure_time;
    int passengers = shm->buses[g_bus_id].passenger_count;
    int at_capacity = (passengers >= BUS_CAPACITY);
    sem_unlock(SEM_SHM_MUTEX);
    
    /*
    Depart if: 
    1) at capacity, 
    2) scheduled time reached, 
    3) or early departure ordered
    */
    if (at_capacity) {
        log_driver(LOG_INFO, "Bus %d: Departing - at capacity", g_bus_id);
        return 1;
    }
    
    if (depart_time > 0 && now >= depart_time) {
        log_driver(LOG_INFO, "Bus %d: Departing - scheduled time reached", g_bus_id);
        return 1;
    }
    
    if (g_early_departure && passengers > 0) {
        log_driver(LOG_INFO, "Bus %d: Departing early with %d passengers", g_bus_id, passengers);
        g_early_departure = 0;
        return 1;
    }
    
    return 0;
}

// ======= MAIN =========

int main(int argc, char *argv[]) {
    // Parse bus ID from cl arg.
    if (argc > 1) {
        g_bus_id = atoi(argv[1]);
    }
    
    printf("[DRIVER %d] Starting (PID=%d)\n", g_bus_id, getpid());
    fflush(stdout);
    
    // Seed random number generator
    srand(time(NULL) ^ getpid());
    
    setup_signals();
    
    // Attach to existing IPC resources
    if (ipc_attach_all() != 0) {
        fprintf(stderr, "[DRIVER %d] Failed to attach to IPC resources\n", g_bus_id);
        exit(EXIT_FAILURE);
    }
    
    // Get shared memory pointer
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "[DRIVER %d] Failed to get shared memory\n", g_bus_id);
        exit(EXIT_FAILURE);
    }
    
    // Register this driver and initialize bus
    sem_lock(SEM_SHM_MUTEX);
    shm->driver_pids[g_bus_id] = getpid();
    shm->buses[g_bus_id].at_station = true;
    shm->buses[g_bus_id].boarding_open = true;
    shm->buses[g_bus_id].passenger_count = 0;
    shm->buses[g_bus_id].bike_count = 0;
    shm->buses[g_bus_id].entering_count = 0;
    shm->buses[g_bus_id].departure_time = time(NULL) + BOARDING_INTERVAL;
    
    // Set first bus as active for boarding
    if (g_bus_id == 0) {
        shm->active_bus_id = 0;
    }
    sem_unlock(SEM_SHM_MUTEX);
    
    log_driver(LOG_INFO, "Bus %d driver started (PID=%d)", g_bus_id, getpid());
    
    // Main driver loop
    while (g_running) {
        // Check for shutdown
        if (check_shutdown(shm)) {
            log_driver(LOG_INFO, "Bus %d: Shutdown detected", g_bus_id);
            break;
        }
        
        // Check for dispatcher commands
        check_dispatcher_commands(shm);
        
        // Only process boarding if this is the active bus!
        sem_lock(SEM_SHM_MUTEX);
        int am_active = (shm->active_bus_id == g_bus_id);
        int at_station = shm->buses[g_bus_id].at_station;
        sem_unlock(SEM_SHM_MUTEX);
        
        if (!at_station) {
            // Bus is en route, wait, TODO: ASK IF OKAY
            usleep(100000);
            continue;
        }
        
        if (!am_active) {
            // Not the active bus, wait, TODO: ASK IF OKAY
            usleep(100000);
            continue;
        }
        
        // Check for boarding requests (non-blocking)
        boarding_msg_t request;
        ssize_t ret = msg_recv_boarding(&request, MSG_BOARD_REQUEST, IPC_NOWAIT);
        
        if (ret > 0) {
            // Process the boarding request
            process_boarding_request(shm, &request);
        }
        
        // Check if it's time to depart
        if (should_depart(shm)) {
            // Switch to next bus before departing
            sem_lock(SEM_SHM_MUTEX);
            int next_bus = (g_bus_id + 1) % MAX_BUSES;
            
            // Find next available bus at station
            for (int i = 0; i < MAX_BUSES; i++) {
                int check_bus = (g_bus_id + 1 + i) % MAX_BUSES;
                if (shm->buses[check_bus].at_station) {
                    next_bus = check_bus;
                    break;
                }
            }
            shm->active_bus_id = next_bus;
            sem_unlock(SEM_SHM_MUTEX);
            
            log_driver(LOG_INFO, "Bus %d: Switching active bus to %d", g_bus_id, next_bus);
            
            // Departing
            depart_bus(shm);
        }
        
        // 50ms wait time for kurcze debug TODO: CHANGE IT SO IT DOES NOT REQUIRE THAT
        usleep(50000);
    }
    
    // Cleanup
    log_driver(LOG_INFO, "Bus %d driver shutting down", g_bus_id);
    
    sem_lock(SEM_SHM_MUTEX);
    shm->driver_pids[g_bus_id] = 0;
    shm->buses[g_bus_id].boarding_open = false;
    sem_unlock(SEM_SHM_MUTEX);
    
    ipc_detach_all();
    
    printf("[DRIVER %d] Terminated\n", g_bus_id);
    return 0;
}
