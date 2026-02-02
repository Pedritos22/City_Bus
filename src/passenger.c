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
#include <pthread.h>
#include <sys/msg.h>



static volatile sig_atomic_t g_running = 1;
static passenger_info_t g_info;

/* Child thread management */
static pthread_t g_child_thread;
static volatile int g_child_boarded = 0;
static volatile int g_adult_boarded = 0;
static pthread_mutex_t g_board_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_board_cond = PTHREAD_COND_INITIALIZER;



static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGINT, &sa, NULL) == -1) perror("sigaction SIGINT");
    if (sigaction(SIGTERM, &sa, NULL) == -1) perror("sigaction SIGTERM");
}



static void* child_thread_func(void *arg) {
    int child_age = *(int*)arg;
    
    log_passenger(LOG_INFO, "PID %d: Child (age=%d%s) thread started, accompanying adult",
                 g_info.pid, child_age, g_info.is_vip ? ", VIP" : "");
    
    /* Wait for adult to board */
    pthread_mutex_lock(&g_board_mutex);
    while (!g_adult_boarded && g_running) {
        /* pthread_cond_wait blocks without busy waiting */
        pthread_cond_wait(&g_board_cond, &g_board_mutex);
    }
    pthread_mutex_unlock(&g_board_mutex);
    
    if (g_adult_boarded) {
        g_child_boarded = 1;
        log_passenger(LOG_INFO, "PID %d: Child (age=%d) boarded with adult on bus %d",
                     g_info.pid, child_age, g_info.assigned_bus);
    } else {
        log_passenger(LOG_WARN, "PID %d: Child (age=%d) could not board - adult did not board",
                     g_info.pid, child_age);
    }
    
    return NULL;
}

static int start_child_thread(void) {
    if (!g_info.has_child_with) {
        return 0;
    }
    
    /* Create child thread
     * pthread_create spawns a new thread within this process */
    int ret = pthread_create(&g_child_thread, NULL, child_thread_func, &g_info.child_age);
    if (ret != 0) {
        perror("pthread_create for child thread");
        return -1;
    }
    
    log_passenger(LOG_INFO, "PID %d (Adult, age=%d): Started child thread for child (age=%d)",
                 g_info.pid, g_info.age, g_info.child_age);
    
    return 0;
}

static void wait_for_child_thread(void) {
    if (!g_info.has_child_with) {
        return;
    }
    
    /* Signal child thread that adult has finished */
    pthread_mutex_lock(&g_board_mutex);
    pthread_cond_signal(&g_board_cond);
    pthread_mutex_unlock(&g_board_mutex);
    
    /* Wait for child thread to finish
     * pthread_join blocks until the thread terminates */
    pthread_join(g_child_thread, NULL);
}



static void init_passenger(void) {
    g_info.pid = getpid();
    
    /* Generate random age - only adults are spawned as processes
     * Children are threads within adult processes */
    g_info.age = ADULT_MIN_AGE + rand() % (MAX_AGE - ADULT_MIN_AGE + 1);
    
    /* Adults are never children themselves */
    g_info.is_child = false;
    
    /* VIP status (~1%) */
    g_info.is_vip = (rand() % 100) < VIP_PERCENT;
    
    /* Bicycle ownership */
    g_info.has_bike = (rand() % 100) < BIKE_PERCENT;
    
    /* Determine if this adult brings a child */
    g_info.has_child_with = (rand() % 100) < ADULT_WITH_CHILD_PERCENT;
    
    if (g_info.has_child_with) {
        /* Generate child age (under CHILD_AGE_LIMIT) */
        g_info.child_age = MIN_AGE + rand() % (CHILD_AGE_LIMIT - MIN_AGE);
        g_info.seat_count = 2;  /* Adult + child = 2 seats */
        
        /* Adult with child cannot have a bike (for simplicity) */
        g_info.has_bike = false;
    } else {
        g_info.child_age = 0;
        g_info.seat_count = 1;  /* Just the adult */
    }
    
    /* VIP passengers (and their children) already have tickets */
    g_info.has_ticket = g_info.is_vip;
    
    /* No assigned bus yet */
    g_info.assigned_bus = -1;
}



static int purchase_ticket(shm_data_t *shm) {
    /* Mark as in office */
    sem_lock(SEM_SHM_MUTEX);
    shm->passengers_in_office++;
    sem_unlock(SEM_SHM_MUTEX);
    
    log_passenger(LOG_INFO, "PID %d (Age=%d%s): Queuing at ticket office",
                 g_info.pid, g_info.age,
                 g_info.has_child_with ? ", with child" : "");
    
    /* Prepare ticket request */
    ticket_msg_t request;
    memset(&request, 0, sizeof(request));
    request.mtype = MSG_TICKET_REQUEST;
    request.passenger = g_info;
    request.approved = false;
    
    /* Limit outstanding ticket requests to avoid msg queue deadlock */
    if (sem_lock(SEM_TICKET_QUEUE_SLOTS) == -1) {
        /* IPC removed - simulation ending */
        sem_lock(SEM_SHM_MUTEX);
        shm->passengers_in_office--;
        sem_unlock(SEM_SHM_MUTEX);
        return 0;
    }

    /* Send request to ticket office */
    if (msg_send_ticket(&request) == -1) {
        log_passenger(LOG_ERROR, "PID %d: Failed to send ticket request", g_info.pid);
        
        sem_unlock(SEM_TICKET_QUEUE_SLOTS);

        sem_lock(SEM_SHM_MUTEX);
        shm->passengers_in_office--;
        sem_unlock(SEM_SHM_MUTEX);
        return 0;
    }
    
    /* Wait for response from dedicated response queue (mtype = our PID) */
    ticket_msg_t response;
    ssize_t ret = msg_recv_ticket_resp(&response, g_info.pid, 0);
    
    if (ret == -1) {
        if (errno == EINTR || errno == EIDRM || errno == EINVAL) {
            sem_lock(SEM_SHM_MUTEX);
            shm->passengers_in_office--;
            sem_unlock(SEM_SHM_MUTEX);
            return 0;
        }
        log_passenger(LOG_ERROR, "PID %d: Failed to receive ticket response", g_info.pid);
        
        sem_lock(SEM_SHM_MUTEX);
        shm->passengers_in_office--;
        sem_unlock(SEM_SHM_MUTEX);
        return 0;
    }
    
    if (response.approved) {
        g_info.has_ticket = true;
        log_passenger(LOG_INFO, "PID %d (Age=%d%s): Ticket purchased (covers %d seat%s)",
                     g_info.pid, g_info.age,
                     g_info.has_child_with ? ", with child" : "",
                     g_info.seat_count,
                     g_info.seat_count > 1 ? "s" : "");
        return 1;
    } else {
        log_passenger(LOG_WARN, "PID %d: Ticket denied", g_info.pid);
        return 0;
    }
}



static int enter_station(shm_data_t *shm) {
    /* Check if station is open */
    sem_lock(SEM_SHM_MUTEX);
    int station_open = shm->station_open;
    sem_unlock(SEM_SHM_MUTEX);
    
    if (!station_open) {
        /* Station closed means end of simulation */
        log_passenger(LOG_WARN, "PID %d: Station is closed, cannot enter", g_info.pid);
        return 0;
    }
    
    /* Wait on station entry semaphore */
    if (sem_lock(SEM_STATION_ENTRY) == -1) {
        /* IPC removed - simulation ending */
        return 0;
    }
    
    if (sem_lock(SEM_SHM_MUTEX) == -1) {
        sem_unlock(SEM_STATION_ENTRY);
        return 0;
    }
    station_open = shm->station_open;
    
    if (station_open) {
        /* Count all people entering (adult + child if present) */
        shm->passengers_waiting += g_info.seat_count;
        sem_unlock(SEM_SHM_MUTEX);
        
        sem_unlock(SEM_STATION_ENTRY);
        
        if (g_info.has_child_with) {
            log_passenger(LOG_INFO, "PID %d (Adult age=%d, Child age=%d): Entered station together",
                         g_info.pid, g_info.age, g_info.child_age);
        } else {
            log_passenger(LOG_INFO, "PID %d (Age=%d, Bike=%s, VIP=%s): Entered station",
                         g_info.pid, g_info.age,
                         g_info.has_bike ? "YES" : "NO",
                         g_info.is_vip ? "YES" : "NO");
        }
        return 1;
    } else {
        sem_unlock(SEM_SHM_MUTEX);
        sem_unlock(SEM_STATION_ENTRY);
        return 0;
    }
}



static int attempt_boarding(shm_data_t *shm) {
    /* Find active bus */
    sem_lock(SEM_SHM_MUTEX);
    int active_bus = shm->active_bus_id;
    int boarding_allowed = shm->boarding_allowed;
    sem_unlock(SEM_SHM_MUTEX);
    
    if (active_bus < 0 || !boarding_allowed) {
        log_passenger(LOG_INFO, "PID %d: No bus available for boarding, waiting...", 
                     g_info.pid);
        return -1;
    }
    
    log_passenger(LOG_INFO, "PID %d: Attempting to board bus %d (%d seat%s needed)", 
                 g_info.pid, active_bus, g_info.seat_count,
                 g_info.seat_count > 1 ? "s" : "");
    
    /* Prepare boarding request - VIP passengers use priority message type */
    boarding_msg_t request;
    memset(&request, 0, sizeof(request));
    request.mtype = g_info.is_vip ? MSG_BOARD_REQUEST_VIP : MSG_BOARD_REQUEST;
    request.passenger = g_info;
    request.bus_id = active_bus;
    request.approved = false;
    
    /* Limit outstanding boarding requests to avoid msg queue deadlock */
    if (sem_lock(SEM_BOARDING_QUEUE_SLOTS) == -1) {
        /* IPC removed - simulation ending */
        return -1;
    }

    /* Send request to driver */
    if (msg_send_boarding(&request) == -1) {
        log_passenger(LOG_ERROR, "PID %d: Failed to send boarding request", g_info.pid);
        sem_unlock(SEM_BOARDING_QUEUE_SLOTS);
        return -1;
    }
    
    /* Wait for response from dedicated response queue (mtype = our PID) */
    boarding_msg_t response;
    ssize_t ret = msg_recv_boarding_resp(&response, g_info.pid, 0);
    
    if (ret == -1) {
        sem_unlock(SEM_BOARDING_QUEUE_SLOTS);
        if (errno == EINTR || errno == EIDRM || errno == EINVAL) {
            return -1;
        }
        log_passenger(LOG_ERROR, "PID %d: Failed to receive boarding response", g_info.pid);
        return -1;
    }
    
    /* Response received, unlock semaphore (request processed) */
    sem_unlock(SEM_BOARDING_QUEUE_SLOTS);
    
    if (response.approved) {
        g_info.assigned_bus = response.bus_id;
        
        /* Signal child thread that we boarded */
        pthread_mutex_lock(&g_board_mutex);
        g_adult_boarded = 1;
        pthread_cond_signal(&g_board_cond);
        pthread_mutex_unlock(&g_board_mutex);
        
        if (g_info.has_child_with) {
            log_passenger(LOG_INFO, "PID %d (Adult age=%d, Child age=%d): BOARDED bus %d together",
                         g_info.pid, g_info.age, g_info.child_age, response.bus_id);
        } else {
            log_passenger(LOG_INFO, "PID %d (Age=%d): BOARDED bus %d",
                         g_info.pid, g_info.age, response.bus_id);
        }
        return 1;
    } else {
        log_passenger(LOG_WARN, "PID %d: Boarding denied - %s",
                     g_info.pid, response.reason);
        
        if (strstr(response.reason, "capacity") != NULL ||
            strstr(response.reason, "not at station") != NULL) {
            return -1;  /* Wait for next bus */
        }
        return 0;
    }
}



int main(void) {
    /* Seed random number generator uniquely for this process */
    srand(time(NULL) ^ getpid() ^ (getpid() << 16));
    

    setup_signals();
    

    init_passenger();
    
    /* Check log mode - only print to stdout if not minimal */
    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    if (!is_minimal) {
        printf("[PASSENGER] PID %d started (Age=%d, VIP=%s, Bike=%s",
               g_info.pid, g_info.age,
               g_info.is_vip ? "YES" : "NO",
               g_info.has_bike ? "YES" : "NO");
        if (g_info.has_child_with) {
            printf(", WITH CHILD age=%d", g_info.child_age);
        }
        printf(")\n");
        fflush(stdout);
    }
    
    /* Attach to existing IPC resources */
    if (ipc_attach_all() != 0) {
        fprintf(stderr, "[PASSENGER %d] Failed to attach to IPC resources\n", g_info.pid);
        exit(EXIT_FAILURE);
    }
    
    /* Get shared memory pointer */
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "[PASSENGER %d] Failed to get shared memory\n", g_info.pid);
        exit(EXIT_FAILURE);
    }
    
    /* Check if simulation is still running and station is open */
    sem_lock(SEM_SHM_MUTEX);
    int running = shm->simulation_running;
    int station_open = shm->station_open;
    sem_unlock(SEM_SHM_MUTEX);
    
    if (!running) {
        log_passenger(LOG_WARN, "PID %d: Simulation not running, exiting", g_info.pid);
        ipc_detach_all();
        return 0;
    }

    /* If station already closed (SIGUSR2 ends simulation), exit early */
    if (!station_open) {
        log_passenger(LOG_INFO, "PID %d: Station closed on arrival - exiting", g_info.pid);
        /* Don't count as left_early since they never entered the counting (created not incremented yet) */
        ipc_detach_all();
        return 0;
    }
    
    if (g_info.has_child_with) {
        log_passenger(LOG_INFO, "PID %d (Adult age=%d, Child age=%d, VIP=%s): Arrived at station",
                     g_info.pid, g_info.age, g_info.child_age,
                     g_info.is_vip ? "YES" : "NO");
    } else {
        log_passenger(LOG_INFO, "PID %d (Age=%d, VIP=%s, Bike=%s): Arrived at station",
                     g_info.pid, g_info.age,
                     g_info.is_vip ? "YES" : "NO",
                     g_info.has_bike ? "YES" : "NO");
    }
    

    if (start_child_thread() != 0) {
        log_passenger(LOG_ERROR, "PID %d: Failed to start child thread", g_info.pid);
        g_info.has_child_with = false;
        g_info.seat_count = 1;
    }
    

    sem_lock(SEM_SHM_MUTEX);
    shm->total_passengers_created += g_info.seat_count;
    shm->adults_created += 1;
    if (g_info.has_child_with) {
        shm->children_created += 1;
    }
    if (g_info.is_vip) {
        shm->vip_people_created += g_info.seat_count;
    }
    sem_unlock(SEM_SHM_MUTEX);
    

    if (!g_info.is_vip) {
        if (!purchase_ticket(shm)) {
            sem_lock(SEM_SHM_MUTEX);
            int running = shm->simulation_running;
            shm->passengers_left_early += g_info.seat_count;
            sem_unlock(SEM_SHM_MUTEX);
            if (running) {
                log_passenger(LOG_ERROR, "PID %d: Could not obtain ticket, leaving", g_info.pid);
            }
            wait_for_child_thread();
            ipc_detach_all();
            return 1;
        }
    } else {
        if (g_info.has_child_with) {
            log_passenger(LOG_INFO, "PID %d: VIP passenger with child - both skip ticket office", g_info.pid);
        } else {
            log_passenger(LOG_INFO, "PID %d: VIP passenger - skipping ticket office", g_info.pid);
        }
    }
    
    /* Check if station closed while buying ticket */
    sem_lock(SEM_SHM_MUTEX);
    int station_closed_now = shm->station_closed;
    sem_unlock(SEM_SHM_MUTEX);
    
    if (station_closed_now) {
        sem_lock(SEM_SHM_MUTEX);
        shm->passengers_left_early += g_info.seat_count;
        sem_unlock(SEM_SHM_MUTEX);
        wait_for_child_thread();
        ipc_detach_all();
        return 1;
    }
    

    int enter_attempts = 0;
    while (!enter_station(shm) && g_running && enter_attempts < 10) {
        enter_attempts++;
        if (!log_is_perf_mode()) {
            sleep(1);
        }
    }
    
    if (enter_attempts >= 10) {
        sem_lock(SEM_SHM_MUTEX);
        int running = shm->simulation_running;
        shm->passengers_left_early += g_info.seat_count;
        sem_unlock(SEM_SHM_MUTEX);
        if (running) {
            log_passenger(LOG_ERROR, "PID %d: Could not enter station, leaving", g_info.pid);
        }
        wait_for_child_thread();
        ipc_detach_all();
        return 1;
    }
    

    int boarded = 0;
    int board_attempts = 0;
    
    while (!boarded && g_running) {
        /* Check if simulation is still running or boarding is blocked */
        sem_lock(SEM_SHM_MUTEX);
        running = shm->simulation_running;
        int boarding_allowed = shm->boarding_allowed;
        sem_unlock(SEM_SHM_MUTEX);
        
        if (!running || !boarding_allowed) {
            if (running) {
                log_passenger(LOG_INFO, "PID %d: Boarding no longer allowed, leaving station", g_info.pid);
            }
            break;
        }
        
        int result = attempt_boarding(shm);
        
        if (result == 1) {
            boarded = 1;
        } else {
            /* result == -1 (no bus) or result == 0 (denied) - keep trying */
            board_attempts++;
            log_passenger(LOG_INFO, "PID %d: Waiting for next bus (attempt %d)",
                         g_info.pid, board_attempts);
            if (!log_is_perf_mode()) {
                sleep(1);
            }
        }
    }
    

    if (boarded) {
        if (g_info.has_child_with) {
            log_passenger(LOG_INFO, "PID %d (Adult age=%d + Child age=%d): Journey complete on bus %d",
                         g_info.pid, g_info.age, g_info.child_age, g_info.assigned_bus);
        } else {
            log_passenger(LOG_INFO, "PID %d (Age=%d): Journey complete on bus %d",
                         g_info.pid, g_info.age, g_info.assigned_bus);
        }
    } else {
        sem_lock(SEM_SHM_MUTEX);
        int running = shm->simulation_running;
        shm->passengers_waiting -= g_info.seat_count;
        if (shm->passengers_waiting < 0) {
            shm->passengers_waiting = 0;
        }
        shm->passengers_left_early += g_info.seat_count;
        sem_unlock(SEM_SHM_MUTEX);
        if (running) {
            log_passenger(LOG_WARN, "PID %d: Could not board any bus, leaving station",
                         g_info.pid);
        }
    }
    

    wait_for_child_thread();
    
    /* Cleanup */
    ipc_detach_all();
    
    /* Use the same log_mode and is_minimal variables defined at the start of main() */
    if (!is_minimal) {
        printf("[PASSENGER] PID %d terminated (boarded=%s%s)\n",
               g_info.pid, boarded ? "YES" : "NO",
               g_info.has_child_with ? ", with child" : "");
    }
    return boarded ? 0 : 1;
}

