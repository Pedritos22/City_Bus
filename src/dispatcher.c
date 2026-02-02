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
static volatile sig_atomic_t g_early_depart = 0;
static volatile sig_atomic_t g_block_station = 0;

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
    g_block_station = 1;
    const char msg[] = "\n[DISPATCHER] SIGUSR2 received - station CLOSED (end simulation)\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
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

static void setup_signals(void) {
    struct sigaction sa;
    
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    /* SIGUSR1 - early departure */
    sa.sa_handler = handle_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction SIGUSR1");
        exit(EXIT_FAILURE);
    }
    
    /* SIGUSR2 - block station */
    sa.sa_handler = handle_sigusr2;
    if (sigaction(SIGUSR2, &sa, NULL) == -1) {
        perror("sigaction SIGUSR2");
        exit(EXIT_FAILURE);
    }
    
    /* SIGINT - shutdown */
    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(EXIT_FAILURE);
    }
    
    /* SIGTERM - shutdown */
    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        exit(EXIT_FAILURE);
    }
    
    /* SIGCHLD - child termination */
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
    shm->spawning_stopped = false;
    shm->station_closed = false;
    
    shm->total_passengers_created = 0;
    shm->passengers_transported = 0;
    shm->passengers_waiting = 0;
    shm->passengers_in_office = 0;
    shm->passengers_left_early = 0;

    shm->adults_created = 0;
    shm->children_created = 0;
    shm->vip_people_created = 0;
    shm->tickets_sold_people = 0;
    shm->tickets_denied = 0;
    shm->boarded_people = 0;
    shm->boarded_vip_people = 0;
    
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
    
    shm->active_bus_id = 0;  /* Bus 0 starts as active */
    
    /* Initialize ticket offices */
    for (int i = 0; i < TICKET_OFFICES; i++) {
        shm->ticket_office_busy[i] = 0;
        shm->ticket_office_pids[i] = 0;
    }
    
    shm->tickets_issued = 0;
    shm->dispatcher_pid = getpid();
}

static void forward_signal_to_drivers(shm_data_t *shm, int sig) {
    sem_lock(SEM_SHM_MUTEX);
    for (int i = 0; i < MAX_BUSES; i++) {
        pid_t driver_pid = shm->driver_pids[i];
        if (driver_pid > 0) {
            if (kill(driver_pid, sig) == -1) {
                if (errno != ESRCH) {
                    perror("forward_signal_to_drivers: kill failed");
                }
            }
        }
    }
    sem_unlock(SEM_SHM_MUTEX);
}

static void process_signals(shm_data_t *shm) {
    if (g_early_depart) {
        g_early_depart = 0;
        
        log_dispatcher(LOG_INFO, "Early departure signal processed - forwarding SIGUSR1 to drivers");
        
        forward_signal_to_drivers(shm, SIGUSR1);
    }
    
    if (g_block_station) {
        sem_lock(SEM_SHM_MUTEX);
        if (!shm->station_closed) {
            shm->station_closed = true;
            shm->station_open = false;
            shm->spawning_stopped = true;   /* main should stop spawning */
            sem_unlock(SEM_SHM_MUTEX);

            log_dispatcher(LOG_WARN, "Station CLOSED - no new entries, waiting passengers can still board");
            printf("[DISPATCHER] SIGUSR2 processed - station closed, waiting passengers will be transported\n");
            fflush(stdout);

            /* Wake up processes blocked on station entry, they will see station_open=false and exit.*/
            sem_setval(SEM_STATION_ENTRY, 1000);
            sem_setval(SEM_TICKET_QUEUE_SLOTS, 1000);
        } else {
            sem_unlock(SEM_SHM_MUTEX);
        }
    }
}

static int all_buses_at_station_and_empty(shm_data_t *shm) {
    for (int i = 0; i < MAX_BUSES; i++) {
        if (!shm->buses[i].at_station) return 0;
        if (shm->buses[i].passenger_count != 0) return 0;
        if (shm->buses[i].entering_count != 0) return 0;
    }
    return 1;
}

/* Check if buses should depart and force departure via SIGUSR1 */
static void check_bus_departures(shm_data_t *shm) {
    time_t now = time(NULL);
    
    sem_lock(SEM_SHM_MUTEX);
    for (int i = 0; i < MAX_BUSES; i++) {
        bus_state_t *bus = &shm->buses[i];
        
        /* Only check active buses at station with passengers */
        if (!bus->at_station || bus->passenger_count == 0) continue;
        if (bus->departure_time == 0) continue;
        
        /* If departure time exceeded by more than 2 seconds, force departure */
        if (now > bus->departure_time + 2) {
            pid_t driver_pid = shm->driver_pids[i];
            if (driver_pid > 0) {
                sem_unlock(SEM_SHM_MUTEX);
                log_dispatcher(LOG_WARN, "Overseer: Bus %d overdue (>2s), forcing departure via SIGUSR1", i);
                kill(driver_pid, SIGUSR1);
                sem_lock(SEM_SHM_MUTEX);
            }
        }
    }
    sem_unlock(SEM_SHM_MUTEX);
}

/* Overseer: detect dead drivers and reassign active_bus_id */
static void check_driver_health(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    
    int active_bus = shm->active_bus_id;
    int active_driver_dead = 0;
    
    /* Check all drivers */
    for (int i = 0; i < MAX_BUSES; i++) {
        pid_t pid = shm->driver_pids[i];
        if (pid > 0) {
            /* kill(pid, 0) checks if process exists without sending signal */
            if (kill(pid, 0) == -1 && errno == ESRCH) {
                /* Driver is dead */
                log_dispatcher(LOG_WARN, "Watchdog: Driver %d (PID %d) is dead, clearing", i, pid);
                shm->driver_pids[i] = 0;
                shm->buses[i].boarding_open = false;
                
                if (i == active_bus) {
                    active_driver_dead = 1;
                }
            }
        }
    }
    
    /* If active driver died, find a new active bus */
    if (active_driver_dead || (active_bus >= 0 && shm->driver_pids[active_bus] == 0)) {
        int new_active = -1;
        
        /* Find first live driver at station */
        for (int i = 0; i < MAX_BUSES; i++) {
            if (shm->driver_pids[i] > 0 && shm->buses[i].at_station) {
                new_active = i;
                break;
            }
        }
        
        if (new_active >= 0) {
            shm->active_bus_id = new_active;
            /* Reset departure time for new active bus */
            int boarding_interval = log_is_perf_mode() ? 1 : BOARDING_INTERVAL;
            shm->buses[new_active].departure_time = time(NULL) + boarding_interval;
            shm->buses[new_active].boarding_open = true;
            sem_unlock(SEM_SHM_MUTEX);
            log_dispatcher(LOG_WARN, "Watchdog: Reassigned active bus to %d (driver PID %d)", 
                          new_active, shm->driver_pids[new_active]);
        } else {
            /* No live driver at station - set to -1, passengers will wait */
            shm->active_bus_id = -1;
            sem_unlock(SEM_SHM_MUTEX);
            log_dispatcher(LOG_WARN, "Watchdog: No live drivers at station, active_bus_id = -1");
        }
    } else {
        sem_unlock(SEM_SHM_MUTEX);
    }
}

static void print_status(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    
    int station_open = shm->station_open;
    int boarding_allowed = shm->boarding_allowed;
    int early_depart = shm->early_departure_flag;
    int created = shm->total_passengers_created;
    int transported = shm->passengers_transported;
    int waiting = shm->passengers_waiting;
    int in_office = shm->passengers_in_office;
    int tickets = shm->tickets_issued;
    int active_bus = shm->active_bus_id;
    sem_unlock(SEM_SHM_MUTEX);

    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    if (is_minimal) {
        printf("STATUS: created=%d transported=%d waiting=%d in_office=%d tickets=%d\n",
               created, transported, waiting, in_office, tickets);
        fflush(stdout);
    } else {
        log_dispatcher(LOG_INFO,
                      "STATUS station=%s boarding=%s early=%s created=%d transported=%d waiting=%d in_office=%d tickets=%d active_bus=%d",
                      station_open ? "OPEN" : "CLOSED",
                      boarding_allowed ? "ALLOWED" : "BLOCKED",
                      early_depart ? "YES" : "NO",
                      created, transported, waiting, in_office, tickets, active_bus);
    }
}

static int check_simulation_end(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int done = !shm->simulation_running;
    int stop = shm->spawning_stopped;
    int waiting = shm->passengers_waiting;
    int in_office = shm->passengers_in_office;
    int buses_done = all_buses_at_station_and_empty(shm);
    sem_unlock(SEM_SHM_MUTEX);

    if (done) return 1;

    if (stop && waiting <= 0 && in_office <= 0 && buses_done) {
        return 1;
    }

    return 0;
}

static void print_final_stats(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int created = shm->total_passengers_created;
    int transported = shm->passengers_transported;
    int waiting = shm->passengers_waiting;
    int in_office = shm->passengers_in_office;
    int left_early = shm->passengers_left_early;
    int tickets = shm->tickets_issued;
    int adults = shm->adults_created;
    int children = shm->children_created;
    int vip_created = shm->vip_people_created;
    int sold_people = shm->tickets_sold_people;
    int denied = shm->tickets_denied;
    int boarded = shm->boarded_people;
    int boarded_vip = shm->boarded_vip_people;
    int on_bus = 0;
    for (int i = 0; i < MAX_BUSES; i++) {
        on_bus += shm->buses[i].passenger_count;
    }
    sem_unlock(SEM_SHM_MUTEX);

    int sum = transported + waiting + in_office + on_bus + left_early;
    if (created != sum) {
        log_dispatcher(LOG_WARN, "STATS INCONSISTENCY: created=%d but transported+waiting+in_office+on_bus+left_early=%d (diff=%d)",
                       created, sum, created - sum);
        log_stats("WARNING: created=%d vs transported+waiting+in_office+on_bus+left_early=%d (diff=%d)", created, sum, created - sum);
    }

    printf("\n========== FINAL STATS ==========\n");
    printf("Created people: %d (adults=%d, children=%d, vip_people=%d)\n", created, adults, children, vip_created);
    printf("Tickets issued: %d (people covered=%d, denied=%d)\n", tickets, sold_people, denied);
    printf("Boarded people: %d (vip_people=%d)\n", boarded, boarded_vip);
    printf("Transported people: %d\n", transported);
    printf("Left early (station closed): %d\n", left_early);
    printf("Remaining: waiting=%d in_office=%d\n", waiting, in_office);
    printf("================================\n\n");

    log_dispatcher(LOG_INFO,
        "STATS created=%d adults=%d children=%d vip_people=%d tickets_issued=%d tickets_people=%d denied=%d boarded=%d boarded_vip=%d transported=%d left_early=%d waiting=%d in_office=%d",
        created, adults, children, vip_created, tickets, sold_people, denied, boarded, boarded_vip, transported, left_early, waiting, in_office);

    log_stats("========== FINAL STATISTICS ==========");
    log_stats("Created people: %d (adults=%d, children=%d, vip_people=%d)", created, adults, children, vip_created);
    log_stats("Tickets issued: %d (people covered=%d, denied=%d)", tickets, sold_people, denied);
    log_stats("Boarded people: %d (vip_people=%d)", boarded, boarded_vip);
    log_stats("Transported people: %d", transported);
    log_stats("Left early (station closed): %d", left_early);
    log_stats("Remaining: waiting=%d in_office=%d", waiting, in_office);
    if (on_bus > 0) {
        log_stats("Still on buses: %d", on_bus);
    }
    log_stats("Consistency: created=%d, transported+waiting+in_office+on_bus+left_early=%d", created, sum);
    log_stats("======================================");
    log_dispatcher(LOG_INFO, "Final statistics written to stats.log");
}

int main(void) {
    // Initialize logging.
    if (log_init() != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        exit(EXIT_FAILURE);
    }
    
    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    if (!is_minimal) {
        printf("[DISPATCHER] Starting (PID=%d)\n", getpid());
        fflush(stdout);
    }
    
    setup_signals();
    if (ipc_create_all() != 0) {
        fprintf(stderr, "Failed to create IPC resources\n");
        exit(EXIT_FAILURE);
    }
    
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "Failed to get shared memory\n");
        ipc_cleanup_all();
        exit(EXIT_FAILURE);
    }
    
    init_shared_state(shm);
    
    log_dispatcher(LOG_INFO, "Dispatcher started and IPC resources created");
    log_dispatcher(LOG_INFO, "DISPATCHER_PID=%d - Send SIGUSR1 for early departure, SIGUSR2 to CLOSE station (end simulation)", getpid());
    
    if (!is_minimal) {
        printf("[DISPATCHER] Ready - IPC resources initialized\n");
        printf("[DISPATCHER] DISPATCHER_PID=%d\n", getpid());
        printf("[DISPATCHER] Send SIGUSR1 to PID %d for early departure\n", getpid());
        printf("[DISPATCHER] Send SIGUSR2 to PID %d to CLOSE station (end simulation)\n", getpid());
        fflush(stdout);
    } else {
        printf("[DISPATCHER] DISPATCHER_PID=%d\n", getpid());
        fflush(stdout);
    }
    
    int status_counter = 0;
    int health_counter = 0;
    while (g_running) {
        process_signals(shm);
        
        /* Watchdog: detect dead drivers and reassign active bus */
        check_driver_health(shm);
        
        /* Overseer: force departure if buses are overdue */
        check_bus_departures(shm);
        
        /* Periodically check queue health */
        if (++health_counter >= 10) {
            ipc_check_queue_health();
            health_counter = 0;
        }
        
        if (!is_minimal) {
            print_status(shm);
        } else {
            status_counter++;
            if (status_counter >= 3) {
                print_status(shm);
                status_counter = 0;
            }
        }
        if (check_simulation_end(shm)) {
            log_dispatcher(LOG_INFO, "Simulation complete - initiating shutdown");
            break;
        }
        if (!log_is_perf_mode()) {
            sleep(DISPATCHER_INTERVAL);
        } else {
            usleep(10000);
        }
    }
    
    // Shutdown sequence
    log_dispatcher(LOG_INFO, "Dispatcher shutting down...");
    if (sem_lock(SEM_SHM_MUTEX) == 0) {
        shm->simulation_running = false;
        sem_unlock(SEM_SHM_MUTEX);
    }
    log_dispatcher(LOG_INFO, "Waiting for processes to exit gracefully...");
    sleep(2);
    print_status(shm);
    print_final_stats(shm);
    log_dispatcher(LOG_INFO, "Cleaning up IPC resources");
    ipc_detach_all();
    ipc_cleanup_all();
    
    log_dispatcher(LOG_INFO, "Dispatcher terminated successfully");
    log_close();
    
    printf("[DISPATCHER] Terminated\n");
    return 0;
}
