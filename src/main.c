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

#define INITIAL_PASSENGER_CAPACITY 1024

/* Process tracking */
static pid_t g_dispatcher_pid = 0;
static pid_t g_ticket_office_pids[TICKET_OFFICES];
static pid_t g_driver_pids[MAX_BUSES];
static pid_t *g_passenger_pids = NULL;
static int g_passenger_count = 0;
static int g_passenger_capacity = 0;
static int g_passengers_spawned = 0;

static int track_passenger_pid(pid_t pid) {
    if (g_passenger_pids == NULL) {
        g_passenger_capacity = INITIAL_PASSENGER_CAPACITY;
        g_passenger_pids = malloc(g_passenger_capacity * sizeof(pid_t));
        if (g_passenger_pids == NULL) {
            perror("malloc g_passenger_pids");
            return -1;
        }
    }
    
    if (g_passenger_count >= g_passenger_capacity) {
        int new_capacity = g_passenger_capacity * 2;
        pid_t *new_array = realloc(g_passenger_pids, new_capacity * sizeof(pid_t));
        if (new_array == NULL) {
            perror("realloc g_passenger_pids");
            return -1;  /* Keep using old array, don't track this PID */
        }
        g_passenger_pids = new_array;
        g_passenger_capacity = new_capacity;
    }
    
    g_passenger_pids[g_passenger_count++] = pid;
    return 0;
}

static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
    
    const char msg[] = "\n[MAIN] Shutdown signal received, terminating...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    
    if (g_dispatcher_pid > 0) {
        kill(g_dispatcher_pid, SIGTERM);
    }
}

static void handle_sigchld(int sig) {
    (void)sig;
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    
    /* Shutdown signals */
    sa.sa_handler = handle_shutdown;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) perror("sigaction SIGINT");
    if (sigaction(SIGTERM, &sa, NULL) == -1) perror("sigaction SIGTERM");
    
    /* Child termination */
    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) perror("sigaction SIGCHLD");
}

static pid_t spawn_dispatcher(void) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork dispatcher");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process, exec dispatcher */
        execl("./dispatcher", "dispatcher", NULL);
        perror("execl dispatcher");
        _exit(EXIT_FAILURE);
    }
    
    printf("[MAIN] Spawned dispatcher (PID=%d)\n", pid);
    return pid;
}

static pid_t spawn_ticket_office(int office_id) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork ticket_office");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process, exec ticket office */
        char id_str[16];
        snprintf(id_str, sizeof(id_str), "%d", office_id);
        execl("./ticket_office", "ticket_office", id_str, NULL);
        perror("execl ticket_office");
        _exit(EXIT_FAILURE);
    }
    
    printf("[MAIN] Spawned ticket office %d (PID=%d)\n", office_id, pid);
    return pid;
}

static pid_t spawn_driver(int bus_id) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork driver");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process, exec driver */
        char id_str[16];
        snprintf(id_str, sizeof(id_str), "%d", bus_id);
        execl("./driver", "driver", id_str, NULL);
        perror("execl driver");
        _exit(EXIT_FAILURE);
    }
    
    printf("[MAIN] Spawned driver for bus %d (PID=%d)\n", bus_id, pid);
    return pid;
}

static pid_t spawn_passenger(void) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork passenger");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process, exec passenger */
        execl("./passenger", "passenger", NULL);
        perror("execl passenger");
        _exit(EXIT_FAILURE);
    }
    
    return pid;
}


static int wait_for_ipc(int timeout_seconds) {
    int elapsed = 0;
    
    while (elapsed < timeout_seconds) {
        if (ipc_resources_exist()) {
            if (ipc_attach_all() == 0) {
                printf("[MAIN] IPC resources ready\n");
                return 0;
            }
        }
        
        sleep(1);
        elapsed++;
        printf("[MAIN] Waiting for IPC resources... (%d/%d)\n", elapsed, timeout_seconds);
    }
    
    fprintf(stderr, "[MAIN] Timeout waiting for IPC resources\n");
    return -1;
}

static int reap_children(void) {
    int reaped = 0;
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        reaped++;
        
        if (pid == g_dispatcher_pid) {
            printf("[MAIN] Dispatcher terminated\n");
            g_dispatcher_pid = 0;
        } else {
            int found = 0;
            for (int i = 0; i < TICKET_OFFICES; i++) {
                if (pid == g_ticket_office_pids[i]) {
                    printf("[MAIN] Ticket office %d terminated\n", i);
                    g_ticket_office_pids[i] = 0;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                for (int i = 0; i < MAX_BUSES; i++) {
                    if (pid == g_driver_pids[i]) {
                        printf("[MAIN] Driver %d terminated\n", i);
                        g_driver_pids[i] = 0;
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) {
                for (int i = 0; i < g_passenger_count; i++) {
                    if (g_passenger_pids[i] == pid) {
                        g_passenger_pids[i] = g_passenger_pids[--g_passenger_count];
                        break;
                    }
                }
            }
        }
    }
    
    return reaped;
}

static int check_simulation_progress(void) {
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        return 0;
    }
    
    sem_lock(SEM_SHM_MUTEX);
    int transported = shm->passengers_transported;
    int created = shm->total_passengers_created;
    int running = shm->simulation_running;
    int stop_spawning = shm->spawning_stopped;
    int waiting = shm->passengers_waiting;
    int in_office = shm->passengers_in_office;
    sem_unlock(SEM_SHM_MUTEX);
    
    /* Check log mode - only print to stdout if not minimal */
    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    if (!is_minimal) {
        printf("[MAIN] Progress: %d/%d passengers transported\n", transported, created);
    }
    
    if (g_dispatcher_pid == 0) {
        printf("[MAIN] Dispatcher has terminated\n");
        return 0;
    }
    
    if (stop_spawning && waiting <= 0 && in_office <= 0 && transported >= created) {
        printf("[MAIN] Drain complete (spawning stopped)\n");
    }
    
    return running;
}

static void terminate_children(void) {
    printf("[MAIN] Terminating all child processes...\n");
    for (int i = 0; i < g_passenger_count; i++) {
        if (g_passenger_pids[i] > 0) {
            kill(g_passenger_pids[i], SIGTERM);
        }
    }
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
    printf("[MAIN] Waiting for children to exit gracefully...\n");
    sleep(2);
    reap_children();
    /* SIGKILL all that might still be alive */
    for (int i = 0; i < g_passenger_count; i++) {
        if (g_passenger_pids[i] > 0) {
            kill(g_passenger_pids[i], SIGKILL);
        }
    }
    g_passenger_count = 0;
    for (int i = 0; i < TICKET_OFFICES; i++) {
        if (g_ticket_office_pids[i] > 0) {
            kill(g_ticket_office_pids[i], SIGKILL);
        }
    }
    for (int i = 0; i < MAX_BUSES; i++) {
        if (g_driver_pids[i] > 0) {
            kill(g_driver_pids[i], SIGKILL);
        }
    }
    if (g_dispatcher_pid > 0) {
        kill(g_dispatcher_pid, SIGKILL);
    }
    /* Reap all children in one loop*/
    {
        int status;
        pid_t pid;
        while (1) {
            pid = waitpid(-1, &status, 0);
            if (pid > 0) {
                if (pid == g_dispatcher_pid) g_dispatcher_pid = 0;
                continue;
            }
            if (pid == -1 && errno == ECHILD) break;
            if (pid == -1 && errno == EINTR) continue;
            break;
        }
    }
}

static void wait_all_children(void) {
    int status;
    pid_t pid;
    int timeout = 8;
    int elapsed = 0;
    int reaped_count = 0;
    
    printf("[MAIN] Waiting for all children to terminate...\n");
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        reaped_count++;
    }
    
    if (errno == ECHILD && reaped_count == 0) {
        printf("[MAIN] All children terminated\n");
        return;
    }
    while (elapsed < timeout) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            /* Reaped a child, reset timeout */
            reaped_count++;
            elapsed = 0;
        } else if (pid == 0) {
            sleep(1);
            elapsed++;
        } else {
            if (errno == ECHILD) {
                printf("[MAIN] All children terminated (reaped %d total)\n", reaped_count);
                return;
            }
            sleep(1);
            elapsed++;
        }
    }
    
    /* Timeout reached, check if any children still exist */
    pid = waitpid(-1, &status, WNOHANG);
    if (pid == -1 && errno == ECHILD) {
        printf("[MAIN] All children terminated (reaped %d total)\n", reaped_count);
    } else {
        printf("[MAIN] Timeout waiting for children after %d seconds (reaped %d so far)\n", timeout, reaped_count);
        printf("[MAIN] Some processes may still be running - they will exit when simulation_running=false\n");
        printf("[MAIN] Continuing cleanup...\n");
    }
}

static void apply_cli_options(int argc, char *argv[]) {
    /*
     *   --log=verbose|summary|minimal
     *   --log verbose|summary|minimal
     *   --summary (same as --log=summary)
     *   --quiet / -q (same as --log=minimal)
     *
     * We propagate the choice to all child processes via environment variable
     * BUS_LOG_MODE, since child processes are exec()'d.
     */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
            setenv("BUS_LOG_MODE", "minimal", 1);
            continue;
        }
        if (strcmp(arg, "--summary") == 0) {
            setenv("BUS_LOG_MODE", "summary", 1);
            continue;
        }
        if (strncmp(arg, "--log=", 6) == 0) {
            setenv("BUS_LOG_MODE", arg + 6, 1);
            continue;
        }
        if (strcmp(arg, "--log") == 0) {
            if (i + 1 < argc) {
                setenv("BUS_LOG_MODE", argv[i + 1], 1);
                i++;
            } else {
                fprintf(stderr, "[MAIN] Missing value for --log (expected verbose|summary|minimal)\n");
            }
            continue;
        }
        if (strcmp(arg, "--perf") == 0 || strcmp(arg, "--performance") == 0) {
            /* Performance mode: disable artificial sleeps in simulation code */
            setenv("BUS_PERF_MODE", "1", 1);
            continue;
        }
        if (strcmp(arg, "--full") == 0) {
            /* Depart when bus is full (instead of waiting for scheduled time) */
            setenv("BUS_FULL_DEPART", "1", 1);
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("Usage: ./main [--log=verbose|summary|minimal] [--summary] [--quiet|-q]\n");
            printf("             [--perf]  (disable simulated sleeps for performance testing)\n");
            printf("             [--full]  (depart when bus is full, don't wait for scheduled time)\n");
            exit(0);
        }
    }
}

int main(int argc, char *argv[]) {
    printf("========================================\n");
    printf("   SUBURBAN BUS SIMULATION\n");
    printf("========================================\n");

    apply_cli_options(argc, argv);

    printf("Configuration:\n");
    printf("  Buses: %d (capacity: %d passengers, %d bikes)\n", 
           MAX_BUSES, BUS_CAPACITY, BIKE_CAPACITY);
    printf("  Ticket offices: %d\n", TICKET_OFFICES);
    printf("  Passengers: continuous until fork() fails or station closes\n");
    printf("  Boarding interval: %d seconds\n", BOARDING_INTERVAL);
    printf("  VIP percentage: %d%%\n", VIP_PERCENT);
    printf("========================================\n\n");
    
    /* Seed random number generator */
    srand(time(NULL));
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Initialize process tracking arrays */
    memset(g_ticket_office_pids, 0, sizeof(g_ticket_office_pids));
    memset(g_driver_pids, 0, sizeof(g_driver_pids));
    
    /* Create logs directory */
    if (mkdir(LOG_DIR, 0755) == -1 && errno != EEXIST) {
        perror("mkdir logs");
        /* Continue anyway */
    }
    
    /* Clear old log files */
    printf("[MAIN] Clearing old log files...\n");
    unlink(LOG_MASTER);
    unlink(LOG_DISPATCHER);
    unlink(LOG_TICKET_OFFICE);
    unlink(LOG_DRIVER);
    unlink(LOG_PASSENGER);
    unlink(LOG_STATS);
    

    printf("[MAIN] Starting dispatcher...\n");
    g_dispatcher_pid = spawn_dispatcher();
    if (g_dispatcher_pid <= 0) {
        fprintf(stderr, "[MAIN] Failed to start dispatcher\n");
        return EXIT_FAILURE;
    }
    

    printf("[MAIN] Waiting for IPC resources...\n");
    if (wait_for_ipc(10) != 0) {
        fprintf(stderr, "[MAIN] IPC resources not available\n");
        terminate_children();
        return EXIT_FAILURE;
    }
    

    printf("[MAIN] Starting ticket offices...\n");
    for (int i = 0; i < TICKET_OFFICES; i++) {
        g_ticket_office_pids[i] = spawn_ticket_office(i);
        if (g_ticket_office_pids[i] <= 0) {
            fprintf(stderr, "[MAIN] Failed to start ticket office %d\n", i);
        }
    }
    
    /* Give ticket offices time to start */
    usleep(100000);
    

    printf("[MAIN] Starting drivers...\n");
    for (int i = 0; i < MAX_BUSES; i++) {
        g_driver_pids[i] = spawn_driver(i);
        if (g_driver_pids[i] <= 0) {
            fprintf(stderr, "[MAIN] Failed to start driver %d\n", i);
        }
    }
    
    /* Give drivers time to start */
    usleep(100000);
    
    /* Check log mode */
    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    if (!is_minimal) {
        printf("\n[MAIN] System initialized. Spawning passengers...\n\n");
        printf("[MAIN] DISPATCHER_PID=%d\n", g_dispatcher_pid);
        printf("[MAIN] Send SIGUSR1 to PID %d for early departure\n", g_dispatcher_pid);
        printf("[MAIN] Send SIGUSR2 to PID %d to CLOSE station (end simulation)\n", g_dispatcher_pid);
        printf("[MAIN] Send SIGINT (Ctrl+C) to shutdown\n\n");
    } else {
        /* Minimal mode: only print PID */
        printf("[MAIN] DISPATCHER_PID=%d\n", g_dispatcher_pid);
    }
    
    /* Also log dispatcher PID for easy grepping */
    log_master(LOG_INFO, "DISPATCHER_PID=%d", g_dispatcher_pid);
    
    
    while (g_running) {
        shm_data_t *shm = ipc_get_shm();
        if (shm) {
            sem_lock(SEM_SHM_MUTEX);
            int stop_spawning = shm->spawning_stopped;
            sem_unlock(SEM_SHM_MUTEX);
            if (stop_spawning) {
                printf("[MAIN] Spawning stopped by dispatcher (station closed) or previous fork() error\n");
                break;
            }
        }

        pid_t pid = spawn_passenger();
        if (pid == -1) {
            printf("[MAIN] fork() failed - stopping passenger creation\n");
            shm_data_t *shm2 = ipc_get_shm();
            if (shm2) {
                sem_lock(SEM_SHM_MUTEX);
                shm2->spawning_stopped = true;
                sem_unlock(SEM_SHM_MUTEX);
            }
            break;
        }

        g_passengers_spawned++;
        track_passenger_pid(pid);
        if ((g_passengers_spawned % 1000) == 0 && !is_minimal) {
            printf("[MAIN] Spawned %d passenger processes so far\n", g_passengers_spawned);
        }

        reap_children();

        /* Random delay between passenger arrivals (can be tuned in config) */
        if (!log_is_perf_mode()) {
            int delay_ms = MIN_ARRIVAL_MS + rand() % (MAX_ARRIVAL_MS - MIN_ARRIVAL_MS + 1);
            usleep(delay_ms * 1000);
        }
    }
    
    printf("\n[MAIN] Passenger creation stopped. Monitoring simulation...\n\n");
    

    while (g_running) {

        reap_children();
        
        /* Check progress */
        if (!check_simulation_progress()) {
            break;
        }
        
        /* Wait before checking again */
        sleep(5);
    }
    
    printf("\n[MAIN] Simulation complete. Shutting down...\n\n");
    

    
    /* Signal dispatcher to shutdown (it will cleanup IPC) */
    if (g_dispatcher_pid > 0) {
        printf("[MAIN] Signaling dispatcher to shutdown...\n");
        kill(g_dispatcher_pid, SIGTERM);
        sleep(2);
    }
    
    /* Terminate remaining children */
    terminate_children();
    
    /* Wait for all children */
    wait_all_children();
    
    ipc_detach_all();
    ipc_cleanup_all();
    
    printf("\n========================================\n");
    printf("   SIMULATION FINISHED\n");
    printf("========================================\n");
    printf("Check log files in '%s/' for details:\n", LOG_DIR);
    printf("  - master.log\n");
    printf("  - dispatcher.log\n");
    printf("  - ticket_office.log\n");
    printf("  - driver.log\n");
    printf("  - passenger.log\n");
    printf("  - stats.log\n");
    printf("========================================\n");
    
    free(g_passenger_pids);
    g_passenger_pids = NULL;
    
    return 0;
}
