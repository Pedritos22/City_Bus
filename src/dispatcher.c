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
#include <sys/shm.h>
#include <sys/sem.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_early_depart = 0;    // SIGUSR1 flag
static volatile sig_atomic_t g_block_station = 0;   // SIGUSR2 flag

/**  
 * Handler for SIGUSR1, early departure signal.
 * Flag to allow buses to depart early.
*/ 
static void handle_sigusr1(int sig) {
    (void)sig;
    g_early_depart = 1;
    const char msg[] = "\n[DISPATCHER] SIGUSR1 received - early departure enabled\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

/**
 * Handler for SIGUSR2, block station signal.
 * Toggle blocking of station entry and boarding.
 */
static void handle_sigusr2(int sig) {
    (void)sig;
    g_block_station = !g_block_station;
    if (g_block_station) {
        const char msg[] = "\n[DISPATCHER] SIGUSR2 received - station BLOCKED\n";
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    } else {
        const char msg[] = "\n[DISPATCHER] SIGUSR2 received - station UNBLOCKED\n";
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    }
}

/**
 * Handler for SIGINT/SIGTERM, shutdown signal.
 * Sets flag g_running to 0 to ensure a good cleanup.
 */
static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
    const char msg[] = "\n[DISPATCHER] Shutdown signal received\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

static void handle_sigchld(int sig) {
    (void)sig;
}

/**
 * Cleanup of all ipc processes
*/
// void cleanup(int sig) {
//     (void)sig;
//     log_dispatcher(LOG_INFO, "Cleaning up and exiting");
//     ipc_detach_all();
//     ipc_cleanup_all();
//     exit(0);
// }

static void setup_signals(void) {
    struct sigaction sa;
    
    // Initialize sigaction structure
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    // SIGUSR1 - early departure
    sa.sa_handler = handle_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction SIGUSR1");
        exit(EXIT_FAILURE);
    }
    
    // SIGUSR2 - block station
    sa.sa_handler = handle_sigusr2;
    if (sigaction(SIGUSR2, &sa, NULL) == -1) {
        perror("sigaction SIGUSR2");
        exit(EXIT_FAILURE);
    }
    
    // SIGINT - shutdown
    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(EXIT_FAILURE);
    }
    
    // SIGTERM - shutdown
    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        exit(EXIT_FAILURE);
    }
    
    // SIGCHLD - child termination
    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_NOCLDSTOP;  /* Don't notify on stop, only terminate */
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction SIGCHLD");
        exit(EXIT_FAILURE);
    }
}

static void init_shared_state(shm_data_t *shm) {
    shm->simulation_running = true;
    shm->station_open = true;
    shm->boarding_allowed = true;
    shm->early_departure_flag = false;
    
    shm->total_passengers_created = 0;
    shm->passengers_transported = 0;
    shm->passengers_waiting = 0;
    shm->passengers_in_office = 0;
    
    // Initialize all buses
    for (int i = 0; i < MAX_BUSES; i++) {
        shm->buses[i].id = i;
        shm->buses[i].at_station = true;
        shm->buses[i].boarding_open = false;
        shm->buses[i].passenger_count = 0;
        shm->buses[i].bike_count = 0;
        shm->buses[i].entering_count = 0;
        shm->buses[i].departure_time = 0;
        shm->buses[i].return_time = 0;
        shm->driver_pids[i] = 0;
    }
    
    shm->active_bus_id = -1;
    
    // Initialize ticket offices
    for (int i = 0; i < TICKET_OFFICES; i++) {
        shm->ticket_office_busy[i] = 0;
        shm->ticket_office_pids[i] = 0;
    }
    
    shm->tickets_issued = 0;
    shm->dispatcher_pid = getpid();
}

static void process_signals(shm_data_t *shm) {
    // Handle early departure flag
    if (g_early_depart) {
        g_early_depart = 0;
        
        sem_lock(SEM_SHM_MUTEX);
        shm->early_departure_flag = true;
        sem_unlock(SEM_SHM_MUTEX);
        
        log_dispatcher(LOG_INFO, "Early departure signal processed, buses may depart with partial capacity");
        
        // Send dispatch message to driver(s)
        dispatch_msg_t msg;
        msg.mtype = MSG_DISPATCH_DEPART;
        msg.sender_pid = getpid();
        msg.target_bus = -1;  // All buses
        snprintf(msg.details, sizeof(msg.details), "Early departure authorized!");
        msg_send_dispatch(&msg);
    }
    
    // Handle station block flag
    sem_lock(SEM_SHM_MUTEX);
    if (g_block_station) {
        if (shm->station_open) {
            shm->station_open = false;
            shm->boarding_allowed = false;
            sem_unlock(SEM_SHM_MUTEX);
            
            log_dispatcher(LOG_WARN, "Station entry BLOCKED, no new passengers allowed");
            
            // Block the station entry semaphore
            sem_setval(SEM_STATION_ENTRY, 0);
            
            dispatch_msg_t msg;
            msg.mtype = MSG_DISPATCH_BLOCK;
            msg.sender_pid = getpid();
            msg.target_bus = -1;
            snprintf(msg.details, sizeof(msg.details), "Station blocked by dispatcher");
            msg_send_dispatch(&msg);
        } else {
            sem_unlock(SEM_SHM_MUTEX);
        }
    } else {
        if (!shm->station_open) {
            shm->station_open = true;
            shm->boarding_allowed = true;
            sem_unlock(SEM_SHM_MUTEX);
            
            log_dispatcher(LOG_INFO, "Station entry UNBLOCKED - normal operation resumed");
            
            // Restore station entry semaphore
            sem_setval(SEM_STATION_ENTRY, 1);
            
            dispatch_msg_t msg;
            msg.mtype = MSG_DISPATCH_UNBLOCK;
            msg.sender_pid = getpid();
            msg.target_bus = -1;
            snprintf(msg.details, sizeof(msg.details), "Station unblocked by dispatcher");
            msg_send_dispatch(&msg);
        } else {
            sem_unlock(SEM_SHM_MUTEX);
        }
    }
}

static void print_status(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    
    printf("\n========== DISPATCHER STATUS ==========\n");
    printf("Station: %s | Boarding: %s | Early Depart: %s\n",
           shm->station_open ? "OPEN" : "CLOSED",
           shm->boarding_allowed ? "ALLOWED" : "BLOCKED",
           shm->early_departure_flag ? "YES" : "NO");
    printf("Passengers: Created=%d, Transported=%d, Waiting=%d, In Office=%d\n",
           shm->total_passengers_created,
           shm->passengers_transported,
           shm->passengers_waiting,
           shm->passengers_in_office);
    printf("Tickets issued: %d\n", shm->tickets_issued);
    
    printf("Buses:\n");
    for (int i = 0; i < MAX_BUSES; i++) {
        printf("  Bus %d: %s | Passengers: %d/%d | Bikes: %d/%d | Entering: %d\n",
               i,
               shm->buses[i].at_station ? "AT STATION" : "EN ROUTE",
               shm->buses[i].passenger_count, BUS_CAPACITY,
               shm->buses[i].bike_count, BIKE_CAPACITY,
               shm->buses[i].entering_count);
    }
    printf("Active bus for boarding: %d\n", shm->active_bus_id);
    printf("==========================================\n\n");
    
    sem_unlock(SEM_SHM_MUTEX);
    fflush(stdout);
}

/**
 * Checker if simulation should end.
 * Ends when all passengers transported and simulation marked as complete.
 * EXCEPTION - SIGINT, ensure good cleanup.
 */
static int check_simulation_end(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int done = !shm->simulation_running;
    int all_transported = (shm->passengers_transported >= shm->total_passengers_created &&
                          shm->total_passengers_created >= MAX_PASSENGERS);
    sem_unlock(SEM_SHM_MUTEX);
    
    return done || all_transported;
}

// ======= MAIN ======== 

int main(void) {
    printf("[DISPATCHER] Starting (PID=%d)\n", getpid());
    fflush(stdout);
    
    // Initialize logging
    if (log_init() != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        exit(EXIT_FAILURE);
    }
    
    setup_signals();
    
    // Create all IPC resources
    if (ipc_create_all() != 0) {
        fprintf(stderr, "Failed to create IPC resources\n");
        exit(EXIT_FAILURE);
    }
    
    // Get shared memory pointer
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "Failed to get shared memory\n");
        ipc_cleanup_all();
        exit(EXIT_FAILURE);
    }
    
    // Initialize shared state
    init_shared_state(shm);
    
    log_dispatcher(LOG_INFO, "Dispatcher started and IPC resources created");
    log_dispatcher(LOG_INFO, "PID=%d - Send SIGUSR1 for early departure, SIGUSR2 to toggle station block", getpid());
    
    printf("[DISPATCHER] Ready - IPC resources initialized\n");
    printf("[DISPATCHER] Send SIGUSR1 to PID %d for early departure\n", getpid());
    printf("[DISPATCHER] Send SIGUSR2 to PID %d to toggle station block\n", getpid());
    fflush(stdout);
    
    // Main dispatcher loop
    while (g_running) {
        // Process any pending signals
        process_signals(shm);
        
        // Print status periodically
        print_status(shm);
        
        // Check for simulation end
        if (check_simulation_end(shm)) {
            log_dispatcher(LOG_INFO, "Simulation complete - initiating shutdown");
            break;
        }
        
        // Sleep to avoid busy waiting
        // sleep() is interruptible by signals!!
        sleep(DISPATCHER_INTERVAL);
    }
    
    // Shutdown sequence
    log_dispatcher(LOG_INFO, "Dispatcher shutting down...");
    
    // Mark simulation as ended
    sem_lock(SEM_SHM_MUTEX);
    shm->simulation_running = false;
    sem_unlock(SEM_SHM_MUTEX);
    
    // Send shutdown message to all processes
    dispatch_msg_t shutdown_msg;
    shutdown_msg.mtype = MSG_DISPATCH_SHUTDOWN;
    shutdown_msg.sender_pid = getpid();
    shutdown_msg.target_bus = -1;
    snprintf(shutdown_msg.details, sizeof(shutdown_msg.details), "System shutdown");
    msg_send_dispatch(&shutdown_msg);
    
    sleep(1);
    
    // Print final status
    print_status(shm);
    
    // Cleanup of IPC resources
    log_dispatcher(LOG_INFO, "Cleaning up and exiting");
    ipc_detach_all();
    ipc_cleanup_all();

    log_dispatcher(LOG_INFO, "Dispatcher terminated successfully");
    log_close();
    
    printf("[DISPATCHER] Terminated\n");
    return 0;
}
