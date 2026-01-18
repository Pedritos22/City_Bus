#include "config.h"
#include "common.h"
#include "ipc.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_running = 1;

static pid_t g_dispatcher_pid = 0;
static pid_t g_ticket_office_pids[TICKET_OFFICES];
static pid_t g_driver_pids[MAX_BUSES];
static pid_t g_passenger_pids[MAX_PASSENGERS];
static int g_passengers_spawned = 0;


/**
 * Handler for SIGINT/SIGTERM - initiates shutdown.
 * Forwards signal to dispatcher for clean shutdown.
 */
static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
    
    const char msg[] = "\n[MAIN] Shutdown signal received, terminating...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    
    // Forward signal to dispatcher
    if (g_dispatcher_pid > 0) {
        kill(g_dispatcher_pid, SIGTERM);
    }
}


/**
 * Handler for SIGCHLD - track child termination.
 */
static void handle_sigchld(int sig) {
    (void)sig;
}

/**
 * Setup signal handlers for main process.
 */
static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    
    // Shutdown signals
    sa.sa_handler = handle_shutdown;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) perror("sigaction SIGINT");
    if (sigaction(SIGTERM, &sa, NULL) == -1) perror("sigaction SIGTERM");
    
    // Child termination
    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) perror("sigaction SIGCHLD");
}


/**
 * Fork and exec the dispatcher process.
 * The dispatcher creates and manages all IPC resources.
 */
static pid_t spawn_dispatcher(void) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork dispatcher");
        return -1;
    }
    
    if (pid == 0) {
        // Child process, execl dispatcgher
        execl("./dispatcher", "dispatcher", NULL);
        perror("execl dispatcher");
        _exit(EXIT_FAILURE);
    }
    
    // Parent, return childID
    printf("[MAIN] Spawned dispatcher (PID=%d)\n", pid);
    return pid;
}

/**
 * Fork and exec a ticket office process.
 * 
 * Ticket office identifier (0 to TICKET_OFFICES-1)
 */
static pid_t spawn_ticket_office(int office_id) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork ticket_office");
        return -1;
    }
    
    if (pid == 0) {
        // Child process, execl ticket office
        char id_str[16];
        snprintf(id_str, sizeof(id_str), "%d", office_id);
        execl("./ticket_office", "ticket_office", id_str, NULL);
        perror("execl ticket_office");
        _exit(EXIT_FAILURE);
    }
    
    printf("[MAIN] Spawned ticket office %d (PID=%d)\n", office_id, pid);
    return pid;
}

/**
 * Fork and exec a driver process.
 * 
 * w/ a bus identifier (0 to MAX_BUSES-1)
 */
static pid_t spawn_driver(int bus_id) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork driver");
        return -1;
    }
    
    if (pid == 0) {
        // Child process, exec driver
        char id_str[16];
        snprintf(id_str, sizeof(id_str), "%d", bus_id);
        execl("./driver", "driver", id_str, NULL);
        perror("execl driver");
        _exit(EXIT_FAILURE);
    }
    
    printf("[MAIN] Spawned driver for bus %d (PID=%d)\n", bus_id, pid);
    return pid;
}

/**
 * Fork and exec a passenger process.
 */
static pid_t spawn_passenger(void) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork passenger");
        return -1;
    }
    
    if (pid == 0) {
        // Child process, exec passenger
        execl("./passenger", "passenger", NULL);
        perror("execl passenger");
        _exit(EXIT_FAILURE);
    }
    
    return pid;
}


/**
 * Wait for IPC resources to be ready.
 * Polls until shared memory is accessible.
 */
static int wait_for_ipc(int timeout_seconds) {
    int elapsed = 0;
    
    while (elapsed < timeout_seconds) {
        if (ipc_resources_exist()) {
            if (ipc_attach_all() == 0) {
                printf("[MAIN] IPC resources ready\n");
                return 0;
            }
        }
        
        sleep(1); // TODO: CHECK IT UP
        elapsed++;
        printf("[MAIN] Waiting for IPC resources... (%d/%d)\n", elapsed, timeout_seconds);
    }
    
    fprintf(stderr, "[MAIN] Timeout waiting for IPC resources\n");
    return -1;
}

/**
 * Gather terminated child processes.
 * Returns number of children gathered.
 */
static int gather_children(void) {
    int gathered = 0;
    int status;
    pid_t pid;
    
    // Wait for any terminated children
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        gathered++;
        
        // Check which process terminated
        if (pid == g_dispatcher_pid) {
            printf("[MAIN] Dispatcher terminated\n");
            g_dispatcher_pid = 0;
        } else {
            // Check ticket offices
            for (int i = 0; i < TICKET_OFFICES; i++) {
                if (pid == g_ticket_office_pids[i]) {
                    printf("[MAIN] Ticket office %d terminated\n", i);
                    g_ticket_office_pids[i] = 0;
                    break;
                }
            }
            
            // Check drivers
            for (int i = 0; i < MAX_BUSES; i++) {
                if (pid == g_driver_pids[i]) {
                    printf("[MAIN] Driver %d terminated\n", i);
                    g_driver_pids[i] = 0;
                    break;
                }
            }
        }
    }
    
    return gathered;
}

/**
 * Check simulation progress.
 * Returns 1 if simulation should continue, 0 if complete.
 */
static int check_simulation_progress(void) {
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        return 0;  // No shm, so just stop
    }
    
    sem_lock(SEM_SHM_MUTEX);
    int transported = shm->passengers_transported;
    int created = shm->total_passengers_created;
    int running = shm->simulation_running;
    sem_unlock(SEM_SHM_MUTEX);
    
    printf("[MAIN] Progress: %d/%d passengers transported\n", transported, created);
    
    // Check if dispatcher is still running
    if (g_dispatcher_pid == 0) {
        printf("[MAIN] Dispatcher has terminated\n");
        return 0;
    }
    
    // Check if all passengers have been transported
    if (created >= MAX_PASSENGERS && transported >= created) {
        printf("[MAIN] All passengers transported\n");
        return 0;
    }
    
    return running;
}

/**
 * Terminate all child processes gracefully.
 */
static void terminate_children(void) {
    printf("[MAIN] Terminating all child processes...\n");
    
    // Send SIGTERM to all children
    for (int i = 0; i < TICKET_OFFICES; i++) {
        if (g_ticket_office_pids[i] > 0) {
            kill(g_ticket_office_pids[i], SIGTERM);
        }
    }
    
    for (int i = 0; i < MAX_BUSES; i++) {
        if (g_driver_pids[i] > 0) {
            kill(g_driver_pids[i], SIGTERM);
        }
    }
    
    if (g_dispatcher_pid > 0) {
        kill(g_dispatcher_pid, SIGTERM);
    }
    
    // Wait for children to terminate TODO: CHECK IF ALL RIGHTY
    sleep(2);
    
    // Forcefully kill any remaining children [worst case scen.]
    for (int i = 0; i < TICKET_OFFICES; i++) {
        if (g_ticket_office_pids[i] > 0) {
            kill(g_ticket_office_pids[i], SIGKILL);
            waitpid(g_ticket_office_pids[i], NULL, 0);
        }
    }
    
    for (int i = 0; i < MAX_BUSES; i++) {
        if (g_driver_pids[i] > 0) {
            kill(g_driver_pids[i], SIGKILL);
            waitpid(g_driver_pids[i], NULL, 0);
        }
    }
    
    if (g_dispatcher_pid > 0) {
        kill(g_dispatcher_pid, SIGKILL);
        waitpid(g_dispatcher_pid, NULL, 0);
    }
}

/**
 * Wait for all remaining children to terminate.
 */
static void wait_all_children(void) {
    int status;
    pid_t pid;
    
    printf("[MAIN] Waiting for all children to terminate...\n");
    
    while ((pid = wait(&status)) > 0) {
        // Czekajka
    }
    
    printf("[MAIN] All children terminated\n");
}

// ============MAIN==============

int main(void) {
    printf("========================================\n");
    printf("   SUBURBAN BUS SIMULATION\n");
    printf("========================================\n");
    printf("Configuration:\n");
    printf("  Buses: %d (capacity: %d passengers, %d bikes)\n", 
           MAX_BUSES, BUS_CAPACITY, BIKE_CAPACITY);
    printf("  Ticket offices: %d\n", TICKET_OFFICES);
    printf("  Passengers: %d\n", MAX_PASSENGERS);
    printf("  Boarding interval: %d seconds\n", BOARDING_INTERVAL);
    printf("  VIP percentage: %d%%\n", VIP_PERCENT);
    printf("========================================\n\n");
    
    // Seed random number
    srand(time(NULL));
    
    setup_signals();
    
    // Initialize process tracking arrays
    memset(g_ticket_office_pids, 0, sizeof(g_ticket_office_pids));
    memset(g_driver_pids, 0, sizeof(g_driver_pids));
    memset(g_passenger_pids, 0, sizeof(g_passenger_pids));
    
    // Create logs directory if does not exist
    if (mkdir(LOG_DIR, 0755) == -1 && errno != EEXIST) {
        perror("mkdir logs");
    }
    
    // Clear old log files
    printf("[MAIN] Clearing old log files...\n");
    unlink(LOG_MASTER);
    unlink(LOG_DISPATCHER);
    unlink(LOG_TICKET_OFFICE);
    unlink(LOG_DRIVER);
    unlink(LOG_PASSENGER);
    
    // Start dispatcher
    printf("[MAIN] Starting dispatcher...\n");
    g_dispatcher_pid = spawn_dispatcher();
    if (g_dispatcher_pid <= 0) {
        fprintf(stderr, "[MAIN] Failed to start dispatcher\n");
        return EXIT_FAILURE;
    }
    
    // Wait for IPC resources
    printf("[MAIN] Waiting for IPC resources...\n");
    if (wait_for_ipc(10) != 0) {
        fprintf(stderr, "[MAIN] IPC resources not available\n");
        terminate_children();
        return EXIT_FAILURE;
    }
    
    // Start ticket offices
    printf("[MAIN] Starting ticket offices...\n");
    for (int i = 0; i < TICKET_OFFICES; i++) {
        g_ticket_office_pids[i] = spawn_ticket_office(i);
        if (g_ticket_office_pids[i] <= 0) {
            fprintf(stderr, "[MAIN] Failed to start ticket office %d\n", i);
        }
    }
    
    // Give ticket offices time to start TODO: CHECK IF AIGHT
    usleep(100000);
    
    // Start drivers
    printf("[MAIN] Starting drivers...\n");
    for (int i = 0; i < MAX_BUSES; i++) {
        g_driver_pids[i] = spawn_driver(i);
        if (g_driver_pids[i] <= 0) {
            fprintf(stderr, "[MAIN] Failed to start driver %d\n", i);
        }
    }
    
    // Give drivers time to start TODO: CHECK IF OKAY
    usleep(100000);
    
    printf("\n[MAIN] System initialized. Spawning passengers...\n\n");
    printf("[MAIN] Dispatcher PID: %d\n", g_dispatcher_pid);
    printf("[MAIN] Send SIGUSR1 to dispatcher for early departure\n");
    printf("[MAIN] Send SIGUSR2 to dispatcher to toggle station block\n");
    printf("[MAIN] Send SIGINT (Ctrl+C) to shutdown\n\n");
    
    // Spawn passengers at random intervals
    while (g_running && g_passengers_spawned < MAX_PASSENGERS) {
        // Spawn a passenger
        pid_t pid = spawn_passenger();
        if (pid > 0) {
            g_passenger_pids[g_passengers_spawned] = pid;
            g_passengers_spawned++;
            printf("[MAIN] Spawned passenger %d/%d (PID=%d)\n",
                   g_passengers_spawned, MAX_PASSENGERS, pid);
        }
        
        // Gather any terminated children
        reap_children();
        
        // Random delay between passenger arrivals TODO: CHECKUP IF CAN DO IT ANOTHER WAY
        int delay_ms = MIN_ARRIVAL_MS + rand() % (MAX_ARRIVAL_MS - MIN_ARRIVAL_MS + 1);
        usleep(delay_ms * 1000);
    }
    
    printf("\n[MAIN] All passengers spawned. Monitoring simulation...\n\n");
    
    // Monitor simulation progress
    while (g_running) {
        // Gather terminated children
        reap_children();
        
        // Check progress
        if (!check_simulation_progress()) {
            break;
        }
        
        // WAIT BEFORE CHECK TODO: ASK IF OKAY
        sleep(5);
    }
    
    printf("\n[MAIN] Simulation complete. Shutting down...\n\n");
    
    // CLEANUP
    
    // Signal dispatcher to shutdown [it cleanes up ipcs]
    if (g_dispatcher_pid > 0) {
        printf("[MAIN] Signaling dispatcher to shutdown...\n");
        kill(g_dispatcher_pid, SIGTERM);
        sleep(2);
    }
    
    terminate_children();
    
    wait_all_children();
    
    ipc_detach_all();
    
    printf("\n========================================\n");
    printf("   SIMULATION FINISHED\n");
    printf("========================================\n");
    printf("Check log files in '%s/' for details:\n", LOG_DIR);
    printf("  - master.log\n");
    printf("  - dispatcher.log\n");
    printf("  - ticket_office.log\n");
    printf("  - driver.log\n");
    printf("  - passenger.log\n");
    printf("========================================\n");
    
    return 0;
}
