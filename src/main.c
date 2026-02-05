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
static int g_test_mode = 0;  /* 0 = normal, 1-8 = test modes */
static int g_max_passengers = 0;  /* 0 = unlimited; when --max_p, use MAX_PASSENGERS */

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
    
    const char msg[] = COLOR_RED "\n[MAIN] Shutdown signal received, terminating...\n" COLOR_RESET;
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
        printf(COLOR_CYAN "[MAIN] Progress: " COLOR_GREEN "%d/%d" COLOR_RESET " passengers transported\n", transported, created);
    }
    
    if (g_dispatcher_pid == 0) {
        printf("[MAIN] Dispatcher has terminated\n");
        return 0;
    }
    
    if (stop_spawning && waiting <= 0 && in_office <= 0 && transported >= created) {
        printf(COLOR_GREEN "[MAIN] Drain complete (spawning stopped)\n" COLOR_RESET);
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

/* Sleep for n seconds using wall-clock time (resistant to EINTR from SIGCHLD etc.) */
static void sleep_seconds(int n) {
    time_t end = time(NULL) + n;
    while (time(NULL) < end) {
        sleep(1);  /* may return early on signal; we loop until wall clock reaches end */
    }
}

/* Run predefined test scenarios (does not modify normal shutdown logic) */
static void run_test(int test_num) {
    shm_data_t *shm = ipc_get_shm();
    
    switch (test_num) {
    case 1:
        /* TEST 1: Kill active driver, verify watchdog reassigns */
        printf("\n[TEST 1] Killing active driver after 5 seconds...\n");
        printf("[TEST 1] Expected: Watchdog detects dead driver, reassigns active_bus_id\n\n");
        sleep_seconds(5);
        if (shm) {
            sem_lock(SEM_SHM_MUTEX);
            int active = shm->active_bus_id;
            pid_t driver_pid = (active >= 0 && active < MAX_BUSES) ? shm->driver_pids[active] : 0;
            sem_unlock(SEM_SHM_MUTEX);
            if (driver_pid > 0) {
                printf("[TEST 1] Killing driver %d (PID %d) with SIGKILL\n", active, driver_pid);
                kill(driver_pid, SIGKILL);
                printf("[TEST 1] Watch dispatcher.log for watchdog reassignment\n\n");
            } else {
                printf("[TEST 1] No active driver found to kill\n\n");
            }
        }
        break;
        
    case 2:
        /* TEST 2: Close station (SIGUSR2), verify remaining passengers transported */
        printf("\n[TEST 2] Sending SIGUSR2 to close station after 5 seconds...\n");
        printf("[TEST 2] Expected: No new passengers enter, existing ones are transported\n\n");
        sleep_seconds(5);
        if (g_dispatcher_pid > 0) {
            printf("[TEST 2] Sending SIGUSR2 to dispatcher (PID %d)\n", g_dispatcher_pid);
            kill(g_dispatcher_pid, SIGUSR2);
            printf("[TEST 2] Station closed. Watching for drain...\n\n");
        }
        break;
        
    case 3:
        /* TEST 3: Force early departures (SIGUSR1 multiple times) */
        printf("\n[TEST 3] Sending SIGUSR1 every 3 seconds (5 times)...\n");
        printf("[TEST 3] Expected: Buses depart early with partial loads\n\n");
        for (int i = 0; i < 5; i++) {
            sleep_seconds(3);
            if (g_dispatcher_pid > 0) {
                printf("[TEST 3] Sending SIGUSR1 #%d to dispatcher\n", i + 1);
                kill(g_dispatcher_pid, SIGUSR1);
            }
        }
        printf("[TEST 3] Check driver.log for early departures\n\n");
        break;
        
    case 4:
        /* TEST 4: Kill a ticket office, verify others still serve tickets */
        printf("\n[TEST 4] Killing ticket office 0 after 5 seconds...\n");
        printf("[TEST 4] Expected: Remaining offices handle load, tickets still issued\n\n");
        sleep_seconds(5);
        if (g_ticket_office_pids[0] > 0) {
            printf("[TEST 4] Killing ticket office 0 (PID %d)\n", g_ticket_office_pids[0]);
            kill(g_ticket_office_pids[0], SIGKILL);
            g_ticket_office_pids[0] = 0;
        } else {
            printf("[TEST 4] Ticket office 0 not running\n");
        }
        break;
        
    case 5:
        /* TEST 5: Stats consistency check after some runtime */
        printf("\n[TEST 5] Running simulation for 15 seconds, then checking stats consistency...\n");
        printf("[TEST 5] Expected: created == transported + waiting + in_office + on_bus + left_early\n\n");
        sleep_seconds(15);
        if (shm) {
            sem_lock(SEM_SHM_MUTEX);
            int created = shm->total_passengers_created;
            int transported = shm->passengers_transported;
            int waiting = shm->passengers_waiting;
            int in_office = shm->passengers_in_office;
            int left_early = shm->passengers_left_early;
            int on_bus = 0;
            for (int i = 0; i < MAX_BUSES; i++) {
                on_bus += shm->buses[i].passenger_count;
            }
            sem_unlock(SEM_SHM_MUTEX);
            
            int sum = transported + waiting + in_office + on_bus + left_early;
            printf("[TEST 5] STATS CHECK:\n");
            printf("  created=%d\n", created);
            printf("  transported=%d + waiting=%d + in_office=%d + on_bus=%d + left_early=%d = %d\n",
                   transported, waiting, in_office, on_bus, left_early, sum);
            if (created == sum) {
                printf("[TEST 5] PASS: Stats are consistent!\n\n");
            } else {
                printf("[TEST 5] FAIL: Inconsistency detected (diff=%d)\n\n", created - sum);
            }
        }
        break;
        
    case 6:
        /* TEST 6: Full ticket queue - SEM_TICKET_QUEUE_SLOTS was set to 0 before spawning in main() */
        printf("\n[TEST 6] Testing FULL TICKET QUEUE scenario...\n");
        printf("[TEST 6] Ticket queue was blocked before spawning; passengers block on sem_lock.\n");
        printf("[TEST 6] Expected: No ticket requests sent, ticket offices idle; recovery when unblocked\n\n");
        
        sleep_seconds(2);  /* Let passengers start and block on SEM_TICKET_QUEUE_SLOTS */
        
        if (shm) {
            int queue_sem_val = sem_getval(SEM_TICKET_QUEUE_SLOTS);
            printf("[TEST 6] SEM_TICKET_QUEUE_SLOTS = %d (blocked)\n", queue_sem_val);
            
            /* Monitor for 10 seconds (queue already blocked) */
            printf("[TEST 6] Monitoring for 10 seconds with blocked ticket queue...\n\n");
            for (int i = 0; i < 10; i++) {
                sleep_seconds(1);
                sem_lock(SEM_SHM_MUTEX);
                int in_office = shm->passengers_in_office;
                int waiting = shm->passengers_waiting;
                int tickets_sold = shm->tickets_sold_people;
                sem_unlock(SEM_SHM_MUTEX);
                
                int queue_sem = sem_getval(SEM_TICKET_QUEUE_SLOTS);
                printf("[TEST 6] t=%2d: in_office=%d, waiting=%d, tickets_sold=%d, queue_sem=%d\n",
                       i + 1, in_office, waiting, tickets_sold, queue_sem);
            }
            
            /* Restore semaphore */
            printf("\n[TEST 6] Restoring SEM_TICKET_QUEUE_SLOTS to %d...\n", MAX_TICKET_QUEUE_REQUESTS);
            sem_setval(SEM_TICKET_QUEUE_SLOTS, MAX_TICKET_QUEUE_REQUESTS);
            
            /* Wait for drain: everyone buys tickets and finishes (in_office==0, waiting==0, all accounted for) */
            printf("[TEST 6] Waiting for all passengers to buy tickets and finish (drain, max 120s)...\n\n");
            int drain_timeout = 120;
            int drained = 0;
            for (int t = 0; t < drain_timeout; t++) {
                sleep_seconds(1);
                sem_lock(SEM_SHM_MUTEX);
                int created = shm->total_passengers_created;
                int in_office = shm->passengers_in_office;
                int waiting = shm->passengers_waiting;
                int transported = shm->passengers_transported;
                int left_early = shm->passengers_left_early;
                int on_bus = 0;
                for (int j = 0; j < MAX_BUSES; j++) {
                    on_bus += shm->buses[j].passenger_count;
                }
                sem_unlock(SEM_SHM_MUTEX);
                
                int sum = transported + waiting + in_office + on_bus + left_early;
                if (t % 5 == 0 || in_office == 0) {
                    printf("[TEST 6] t=%3d: in_office=%d, waiting=%d, transported=%d, left_early=%d, on_bus=%d (created=%d)\n",
                           t, in_office, waiting, transported, left_early, on_bus, created);
                }
                if (in_office == 0 && waiting == 0 && sum == created && created > 0) {
                    printf("\n[TEST 6] Drain complete: all %d passengers finished (transported + left_early + on_bus).\n", created);
                    drained = 1;
                    break;
                }
            }
            if (!drained) {
                printf("\n[TEST 6] Timeout waiting for drain; check logs for stuck passengers.\n");
            }
            printf("\n[TEST 6] Test complete.\n\n");
        }
        break;
        
    case 7:
        /* TEST 7: Full boarding queue - SEM_BOARDING_QUEUE_SLOTS was set to 0 before spawning in main() */
        printf("\n[TEST 7] Testing FULL BOARDING QUEUE scenario...\n");
        printf("[TEST 7] Boarding queue was blocked before spawning; passengers block when requesting board.\n");
        printf("[TEST 7] Expected: No boarding requests sent to driver; recovery when unblocked\n\n");
        
        sleep_seconds(5);  /* Let passengers get tickets and reach boarding request (then block) */
        
        if (shm) {
            int queue_sem_val = sem_getval(SEM_BOARDING_QUEUE_SLOTS);
            printf("[TEST 7] SEM_BOARDING_QUEUE_SLOTS = %d (blocked)\n", queue_sem_val);
            
            /* Monitor for 10 seconds (queue already blocked) */
            printf("[TEST 7] Monitoring for 10 seconds with blocked boarding queue...\n\n");
            for (int i = 0; i < 10; i++) {
                sleep_seconds(1);
                sem_lock(SEM_SHM_MUTEX);
                int waiting = shm->passengers_waiting;
                int boarded = shm->boarded_people;
                int transported = shm->passengers_transported;
                int on_bus = 0;
                for (int j = 0; j < MAX_BUSES; j++) {
                    on_bus += shm->buses[j].passenger_count;
                }
                sem_unlock(SEM_SHM_MUTEX);
                
                int queue_sem = sem_getval(SEM_BOARDING_QUEUE_SLOTS);
                printf("[TEST 7] t=%2d: waiting=%d, on_bus=%d, boarded=%d, transported=%d, queue_sem=%d\n",
                       i + 1, waiting, on_bus, boarded, transported, queue_sem);
            }
            
            /* Restore semaphore */
            printf("\n[TEST 7] Restoring SEM_BOARDING_QUEUE_SLOTS to %d...\n", MAX_BOARDING_QUEUE_REQUESTS);
            sem_setval(SEM_BOARDING_QUEUE_SLOTS, MAX_BOARDING_QUEUE_REQUESTS);
            
            /* Wait for drain: everyone boards or leaves (waiting==0, in_office==0, all accounted for) */
            printf("[TEST 7] Waiting for all passengers to board or leave (drain, max 120s)...\n\n");
            int drain_timeout = 120;
            int drained = 0;
            for (int t = 0; t < drain_timeout; t++) {
                sleep_seconds(1);
                sem_lock(SEM_SHM_MUTEX);
                int created = shm->total_passengers_created;
                int in_office = shm->passengers_in_office;
                int waiting = shm->passengers_waiting;
                int transported = shm->passengers_transported;
                int left_early = shm->passengers_left_early;
                int on_bus = 0;
                for (int j = 0; j < MAX_BUSES; j++) {
                    on_bus += shm->buses[j].passenger_count;
                }
                sem_unlock(SEM_SHM_MUTEX);
                
                int sum = transported + waiting + in_office + on_bus + left_early;
                if (t % 5 == 0 || waiting == 0) {
                    printf("[TEST 7] t=%3d: waiting=%d, in_office=%d, transported=%d, left_early=%d, on_bus=%d (created=%d)\n",
                           t, waiting, in_office, transported, left_early, on_bus, created);
                }
                if (in_office == 0 && waiting == 0 && sum == created && created > 0) {
                    printf("\n[TEST 7] Drain complete: all %d passengers finished (transported + left_early + on_bus).\n", created);
                    drained = 1;
                    break;
                }
            }
            if (!drained) {
                printf("\n[TEST 7] Timeout waiting for drain; check logs for stuck passengers.\n");
            }
            printf("\n[TEST 7] Test complete.\n\n");
        }
        break;
        
    case 8:
        /* TEST 8: Combined stress - both queues were set to 0 before spawning in main() */
        printf("\n[TEST 8] Testing BOTH QUEUES FULL simultaneously...\n");
        printf("[TEST 8] Both ticket and boarding queues were blocked before spawning.\n");
        printf("[TEST 8] Expected: Passengers block at ticket queue; no requests; full recovery when unblocked\n\n");
        
        sleep_seconds(2);  /* Let passengers start and block on SEM_TICKET_QUEUE_SLOTS */
        
        if (shm) {
            printf("[TEST 8] SEM_TICKET_QUEUE_SLOTS=%d, SEM_BOARDING_QUEUE_SLOTS=%d (both blocked)\n",
                   sem_getval(SEM_TICKET_QUEUE_SLOTS), sem_getval(SEM_BOARDING_QUEUE_SLOTS));
            printf("[TEST 8] Monitoring for 15 seconds with both queues blocked...\n\n");
            for (int i = 0; i < 15; i++) {
                sleep_seconds(1);
                sem_lock(SEM_SHM_MUTEX);
                int in_office = shm->passengers_in_office;
                int waiting = shm->passengers_waiting;
                int boarded = shm->boarded_people;
                int transported = shm->passengers_transported;
                int created = shm->total_passengers_created;
                sem_unlock(SEM_SHM_MUTEX);
                
                int ticket_sem = sem_getval(SEM_TICKET_QUEUE_SLOTS);
                int boarding_sem = sem_getval(SEM_BOARDING_QUEUE_SLOTS);
                printf("[TEST 8] t=%2d: created=%d, in_office=%d, waiting=%d, boarded=%d, transported=%d | ticket_sem=%d, boarding_sem=%d\n",
                       i + 1, created, in_office, waiting, boarded, transported, ticket_sem, boarding_sem);
            }
            
            /* Restore both semaphores */
            printf("\n[TEST 8] Restoring both queues...\n");
            sem_setval(SEM_TICKET_QUEUE_SLOTS, MAX_TICKET_QUEUE_REQUESTS);
            sem_setval(SEM_BOARDING_QUEUE_SLOTS, MAX_BOARDING_QUEUE_REQUESTS);
            
            /* Wait for drain: everyone buys tickets, boards or leaves */
            printf("[TEST 8] Waiting for all passengers to finish (drain, max 120s)...\n\n");
            {
                int drain_timeout = 120;
                int drained = 0;
                for (int t = 0; t < drain_timeout; t++) {
                    sleep_seconds(1);
                    sem_lock(SEM_SHM_MUTEX);
                    int created = shm->total_passengers_created;
                    int in_office = shm->passengers_in_office;
                    int waiting = shm->passengers_waiting;
                    int transported = shm->passengers_transported;
                    int left_early = shm->passengers_left_early;
                    int on_bus = 0;
                    for (int j = 0; j < MAX_BUSES; j++) {
                        on_bus += shm->buses[j].passenger_count;
                    }
                    sem_unlock(SEM_SHM_MUTEX);
                    
                    int sum = transported + waiting + in_office + on_bus + left_early;
                    if (t % 5 == 0 || (in_office == 0 && waiting == 0)) {
                        printf("[TEST 8] t=%3d: in_office=%d, waiting=%d, transported=%d, left_early=%d, on_bus=%d (created=%d)\n",
                               t, in_office, waiting, transported, left_early, on_bus, created);
                    }
                    if (in_office == 0 && waiting == 0 && sum == created && created > 0) {
                        printf("\n[TEST 8] Drain complete: all %d passengers finished.\n", created);
                        drained = 1;
                        break;
                    }
                }
                if (!drained) {
                    printf("\n[TEST 8] Timeout waiting for drain.\n");
                }
            }
            
            /* Final stats check */
            sem_lock(SEM_SHM_MUTEX);
            int created = shm->total_passengers_created;
            int transported = shm->passengers_transported;
            int waiting = shm->passengers_waiting;
            int in_office = shm->passengers_in_office;
            int left_early = shm->passengers_left_early;
            int on_bus = 0;
            for (int j = 0; j < MAX_BUSES; j++) {
                on_bus += shm->buses[j].passenger_count;
            }
            sem_unlock(SEM_SHM_MUTEX);
            
            int sum = transported + waiting + in_office + on_bus + left_early;
            printf("\n[TEST 8] FINAL STATS CHECK:\n");
            printf("  created=%d\n", created);
            printf("  transported=%d + waiting=%d + in_office=%d + on_bus=%d + left_early=%d = %d\n",
                   transported, waiting, in_office, on_bus, left_early, sum);
            if (created == sum) {
                printf("[TEST 8] PASS: Stats are consistent after stress test!\n\n");
            } else {
                printf("[TEST 8] FAIL: Inconsistency detected (diff=%d)\n\n", created - sum);
            }
        }
        break;
    
    case 9:
        /* TEST 9: Stop ticket office with SIGSTOP, queue fills up */
        printf("\n[TEST 9] Stopping ticket office 0 with SIGSTOP...\n");
        printf("[TEST 9] Expected: Queue fills up, other offices handle load\n");
        printf("[TEST 9] Then resume with SIGCONT to verify recovery\n\n");

        sleep_seconds(5);  /* Let system stabilize */

        if (g_ticket_office_pids[0] > 0) {
            printf("[TEST 9] Sending SIGSTOP to ticket office 0 (PID %d)\n",
                g_ticket_office_pids[0]);
            kill(g_ticket_office_pids[0], SIGSTOP);

            /* Monitor queue filling */
            for (int i = 0; i < 10; i++) {
                sleep_seconds(1);
                /* Check queue depth, in_office, etc. */
            }

            /* Resume */
            printf("[TEST 9] Resuming ticket office 0 with SIGCONT\n");
            kill(g_ticket_office_pids[0], SIGCONT);

            /* Verify recovery */
            sleep_seconds(10);
        } else {
            printf("[TEST 9] No ticket_office 0\n");
        }
        break;

    case 10:
        /* TEST 10: Stop driver 0 with SIGSTOP, boarding queue fills up */
        printf("\n[TEST 10] Stopping driver 0 with SIGSTOP...\n");
        printf("[TEST 10] Expected: Boarding queue fills; \n");
        printf("[TEST 10] Then resume with SIGCONT to verify recovery\n\n");

        sleep_seconds(5);  /* Let system stabilize */

        if (g_driver_pids[0] > 0) {
            printf("[TEST 10] Sending SIGSTOP to driver 0 (PID %d)\n",
                   g_driver_pids[0]);
            kill(g_driver_pids[0], SIGSTOP);

            /* Monitor boarding queue / waiting */
            for (int i = 0; i < 10; i++) {
                sleep_seconds(1);
                if (shm) {
                    sem_lock(SEM_SHM_MUTEX);
                    int waiting = shm->passengers_waiting;
                    int boarded = shm->boarded_people;
                    int transported = shm->passengers_transported;
                    int on_bus = 0;
                    for (int j = 0; j < MAX_BUSES; j++) {
                        on_bus += shm->buses[j].passenger_count;
                    }
                    sem_unlock(SEM_SHM_MUTEX);
                    int queue_sem = sem_getval(SEM_BOARDING_QUEUE_SLOTS);
                    printf("[TEST 10] t=%2d: waiting=%d, boarded=%d, transported=%d, on_bus=%d, queue_sem=%d\n",
                           i + 1, waiting, boarded, transported, on_bus, queue_sem);
                }
            }

            /* Resume */
            printf("[TEST 10] Resuming driver 0 with SIGCONT\n");
            kill(g_driver_pids[0], SIGCONT);

            /* Verify recovery */
            sleep_seconds(10);
        } else {
            printf("[TEST 10] No driver 0\n");
        }
        break;

    default:
        printf("[TEST] Unknown test number: %d\n", test_num);
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
        if (strcmp(arg, "--max_p") == 0) {
            /* Cap passenger count at MAX_PASSENGERS (from config.h) */
            g_max_passengers = MAX_PASSENGERS;
            continue;
        }
        /* Test modes (side-effect only, don't change core shutdown) */
        if (strcmp(arg, "--test1") == 0) { g_test_mode = 1; continue; }
        if (strcmp(arg, "--test2") == 0) { g_test_mode = 2; continue; }
        if (strcmp(arg, "--test3") == 0) { g_test_mode = 3; continue; }
        if (strcmp(arg, "--test4") == 0) { g_test_mode = 4; continue; }
        if (strcmp(arg, "--test5") == 0) { g_test_mode = 5; continue; }
        if (strcmp(arg, "--test6") == 0) { g_test_mode = 6; continue; }
        if (strcmp(arg, "--test7") == 0) { g_test_mode = 7; continue; }
        if (strcmp(arg, "--test8") == 0) { g_test_mode = 8; continue; }
        if (strcmp(arg, "--test9") == 0) { g_test_mode = 9; continue; }
        if (strcmp(arg, "--test10") == 0) { g_test_mode = 10; continue; }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("Usage: ./main [--log=verbose|summary|minimal] [--summary] [--quiet|-q]\n");
            printf("             [--perf]  (disable simulated sleeps for performance testing)\n");
            printf("             [--full]  (depart when bus is full, don't wait for scheduled time)\n");
            printf("             [--max_p] (cap passengers at MAX_PASSENGERS from config; used with tests)\n");
            printf("\nTest modes:\n");
            printf("  --test1  Kill active driver, verify watchdog reassigns\n");
            printf("  --test2  Close station (SIGUSR2), verify drain\n");
            printf("  --test3  Force early departures (SIGUSR1)\n");
            printf("  --test4  Kill ticket office, verify others handle load\n");
            printf("  --test5  Stats consistency check\n");
            printf("  --test6  Full ticket queue test (block SEM_TICKET_QUEUE_SLOTS)\n");
            printf("  --test7  Full boarding queue test (block SEM_BOARDING_QUEUE_SLOTS)\n");
            printf("  --test8  Combined stress test (both queues full)\n");
            printf("  --test9  Full message queue test for ticket office 0\n");
            printf("  --test10  Full message queue test for driver 0\n");
            exit(0);
        }
    }
}

int main(int argc, char *argv[]) {
    printf(COLOR_CYAN "========================================\n");
    printf("   SUBURBAN BUS SIMULATION\n");
    printf("========================================\n" COLOR_RESET);

    apply_cli_options(argc, argv);

    printf("Configuration:\n");
    printf("  Buses: %d (capacity: %d passengers, %d bikes)\n", 
           MAX_BUSES, BUS_CAPACITY, BIKE_CAPACITY);
    printf("  Ticket offices: %d\n", TICKET_OFFICES);
    if (g_max_passengers > 0) {
        printf("  Passengers: max %d (--max_p, MAX_PASSENGERS from config)\n", g_max_passengers);
    } else {
        printf("  Passengers: continuous until fork() fails or station closes\n");
    }
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
    
    /* Verify dispatcher is still running (e.g. execl didn't fail - run from build dir) */
    reap_children();
    if (g_dispatcher_pid == 0) {
        fprintf(stderr, "[MAIN] Dispatcher exited immediately. Run from the directory that contains the binaries, e.g.:\n");
        fprintf(stderr, "  cd build && ./main [options]\n");
        ipc_detach_all();
        ipc_cleanup_all();
        return EXIT_FAILURE;
    }
    if (kill(g_dispatcher_pid, 0) != 0) {
        fprintf(stderr, "[MAIN] Dispatcher (PID %d) is not running.\n", g_dispatcher_pid);
        ipc_detach_all();
        ipc_cleanup_all();
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
        printf(COLOR_GREEN "\n[MAIN] System initialized. Spawning passengers...\n\n" COLOR_RESET);
        printf(COLOR_CYAN "[MAIN] DISPATCHER_PID=%d\n" COLOR_RESET, g_dispatcher_pid);
        printf(COLOR_YELLOW "[MAIN] Send SIGUSR1 to PID %d for early departure\n" COLOR_RESET, g_dispatcher_pid);
        printf(COLOR_RED "[MAIN] Send SIGUSR2 to PID %d to CLOSE station (end simulation)\n" COLOR_RESET, g_dispatcher_pid);
        printf(COLOR_RED "[MAIN] Send SIGINT (Ctrl+C) to shutdown\n\n" COLOR_RESET);
    } else {
        /* Minimal mode: only print PID */
        printf(COLOR_CYAN "[MAIN] DISPATCHER_PID=%d\n" COLOR_RESET, g_dispatcher_pid);
    }
    
    /* Also log dispatcher PID for easy grepping */
    log_master(LOG_INFO, "DISPATCHER_PID=%d", g_dispatcher_pid);
    
    if (g_test_mode > 0) {
        /* Test mode: spawn up to g_max_passengers (MAX_PASSENGERS when --max_p), run test, then shutdown */
        printf("\n========================================\n");
        printf("   RUNNING TEST %d\n", g_test_mode);
        printf("========================================\n");
        
        /* Block queues BEFORE spawning so passengers block on semaphore (test 6/7/8) */
        if (g_test_mode == 6) {
            printf("[MAIN] Test 6: Blocking ticket queue (SEM_TICKET_QUEUE_SLOTS=0) before spawning.\n");
            sem_setval(SEM_TICKET_QUEUE_SLOTS, 0);
        } else if (g_test_mode == 7) {
            printf("[MAIN] Test 7: Blocking boarding queue (SEM_BOARDING_QUEUE_SLOTS=0) before spawning.\n");
            sem_setval(SEM_BOARDING_QUEUE_SLOTS, 0);
        } else if (g_test_mode == 8) {
            printf("[MAIN] Test 8: Blocking both ticket and boarding queues before spawning.\n");
            sem_setval(SEM_TICKET_QUEUE_SLOTS, 0);
            sem_setval(SEM_BOARDING_QUEUE_SLOTS, 0);
        }
        
        int limit = g_max_passengers > 0 ? g_max_passengers : 50;  /* default 50 if no --max_p */
        if (g_max_passengers > 0) {
            printf("[MAIN] Test mode: spawning %d passengers (MAX_PASSENGERS from config)...\n", limit);
        } else {
            printf("[MAIN] Test mode: spawning %d passengers (use --max_p for MAX_PASSENGERS)...\n", limit);
        }
        printf("[MAIN] (Run from directory containing dispatcher, driver, passenger, ticket_office)\n");
        while (g_passengers_spawned < limit && g_running) {
            pid_t pid = spawn_passenger();
            if (pid == -1) {
                printf("[MAIN] fork() failed after %d passengers\n", g_passengers_spawned);
                break;
            }
            g_passengers_spawned++;
            track_passenger_pid(pid);
            reap_children();
            if (!log_is_perf_mode()) {
                int delay_ms = MIN_ARRIVAL_MS + rand() % (MAX_ARRIVAL_MS - MIN_ARRIVAL_MS + 1);
                usleep(delay_ms * 1000);
            }
        }
        printf("[MAIN] Spawned %d passengers. Running test...\n\n", g_passengers_spawned);
        
        run_test(g_test_mode);
        
        printf(COLOR_GREEN "\n[MAIN] Test %d finished. Shutting down...\n\n" COLOR_RESET, g_test_mode);
    } else {
        /* Normal mode: spawn passengers until station closes, fork fails, or --max_p limit */
        while (g_running) {
            if (g_max_passengers > 0 && g_passengers_spawned >= g_max_passengers) {
                printf("[MAIN] Reached passenger limit %d (--max_p)\n", g_max_passengers);
                /* Stop spawning only; do NOT close station (SIGUSR2) so ticket offices keep serving.
                 * Dispatcher will exit when drain is complete (check_simulation_end). */
                shm_data_t *shm_mp = ipc_get_shm();
                if (shm_mp) {
                    sem_lock(SEM_SHM_MUTEX);
                    shm_mp->spawning_stopped = true;
                    sem_unlock(SEM_SHM_MUTEX);
                    printf("[MAIN] spawning_stopped=true; station stays open until all passengers are done.\n");
                }
                break;
            }
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

            if (!log_is_perf_mode()) {
                int delay_ms = MIN_ARRIVAL_MS + rand() % (MAX_ARRIVAL_MS - MIN_ARRIVAL_MS + 1);
                usleep(delay_ms * 1000);
            }
        }
        
        printf(COLOR_YELLOW "\n[MAIN] Passenger creation stopped. Monitoring simulation...\n\n" COLOR_RESET);
        
        while (g_running) {
            reap_children();
            if (!check_simulation_progress()) {
                break;
            }
            /* When spawning_stopped (--max_p or SIGUSR2), dispatcher may wait for buses_done forever.
             * If drain is complete (all passengers accounted for), signal dispatcher to shutdown. */
            if (g_dispatcher_pid > 0) {
                shm_data_t *shm_d = ipc_get_shm();
                if (shm_d) {
                    sem_lock(SEM_SHM_MUTEX);
                    int stop = shm_d->spawning_stopped;
                    int created = shm_d->total_passengers_created;
                    int waiting = shm_d->passengers_waiting;
                    int in_office = shm_d->passengers_in_office;
                    int transported = shm_d->passengers_transported;
                    int left_early = shm_d->passengers_left_early;
                    int on_bus = 0;
                    for (int j = 0; j < MAX_BUSES; j++) {
                        on_bus += shm_d->buses[j].passenger_count;
                    }
                    sem_unlock(SEM_SHM_MUTEX);
                    int sum = transported + waiting + in_office + on_bus + left_early;
                    if (stop && created > 0 && waiting == 0 && in_office == 0 && sum == created) {
                        printf("[MAIN] Drain complete (%d passengers); signaling dispatcher to shutdown.\n", created);
                        kill(g_dispatcher_pid, SIGTERM);
                    }
                }
            }
            sleep(5);
        }
        
        printf(COLOR_GREEN "\n[MAIN] Simulation complete. Shutting down...\n\n" COLOR_RESET);
    }
    

    
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
    
    printf(COLOR_GREEN "\n========================================\n");
    printf("   SIMULATION FINISHED\n");
    printf("========================================\n" COLOR_RESET);
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
