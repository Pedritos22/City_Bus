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

/*============================================================================
 * GLOBAL STATE
 *============================================================================*/

static volatile sig_atomic_t g_running = 1;
static int g_office_id = 0;

/*============================================================================
 * SIGNAL HANDLERS
 *============================================================================*/

static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
}

static void handle_alarm(int sig) {
    (void)sig;
    /* Just interrupt - no action needed */
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sa.sa_handler = handle_shutdown;
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
    }
    
    sa.sa_handler = handle_alarm;
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("sigaction SIGALRM");
    }
}

/*============================================================================
 * TICKET PROCESSING
 *============================================================================*/

static int validate_passenger(const passenger_info_t *passenger) {
    /* Validate age */
    if (passenger->age < MIN_AGE || passenger->age > MAX_AGE) {
        return 0;
    }
    
    /* Validate PID */
    if (passenger->pid <= 0) {
        return 0;
    }
    
    return 1;
}

static void process_ticket_request(shm_data_t *shm, ticket_msg_t *request) {
    ticket_msg_t response;
    memset(&response, 0, sizeof(response));
    
    /* Set response mtype to passenger's PID for targeted delivery */
    response.mtype = request->passenger.pid;
    response.passenger = request->passenger;
    response.ticket_office_id = g_office_id;
    
    /* Validate passenger data */
    if (!validate_passenger(&request->passenger)) {
        response.approved = false;
        /* Ticket denied still must clear 'in_office' counter */
        sem_lock(SEM_SHM_MUTEX);
        shm->tickets_denied++;
        shm->passengers_in_office--;
        sem_unlock(SEM_SHM_MUTEX);
        log_ticket_office(LOG_WARN, "Office %d: Invalid passenger data from PID %d",
                         g_office_id, request->passenger.pid);
    } else {
        /* Simulate ticket processing time - skipped in performance mode */
        if (!log_is_perf_mode()) {
            sleep(TICKET_PROCESS_TIME);
        }
        
        /* Issue the ticket */
        response.approved = true;
        response.passenger.has_ticket = true;
        
        /* Update shared memory */
        sem_lock(SEM_SHM_MUTEX);
        shm->tickets_issued++;
        shm->tickets_sold_people += request->passenger.seat_count > 0 ? request->passenger.seat_count : 1;
        shm->passengers_in_office--;
        sem_unlock(SEM_SHM_MUTEX);
        
        /* Log ticket issuance with child info if applicable */
        if (request->passenger.has_child_with) {
            log_ticket_office(LOG_INFO, 
                             "Office %d: Ticket issued to adult PID %d (Age=%d) WITH CHILD (Age=%d) - %d seats",
                             g_office_id,
                             request->passenger.pid,
                             request->passenger.age,
                             request->passenger.child_age,
                             request->passenger.seat_count);
        } else {
            log_ticket_office(LOG_INFO, 
                             "Office %d: Ticket issued to passenger PID %d (Age=%d, Bike=%s)",
                             g_office_id,
                             request->passenger.pid,
                             request->passenger.age,
                             request->passenger.has_bike ? "YES" : "NO");
        }
    }
    
    /* Send response back to passenger */
    if (msg_send_ticket(&response) == -1) {
        log_ticket_office(LOG_ERROR, "Office %d: Failed to send ticket response to PID %d",
                         g_office_id, request->passenger.pid);
    }
}

static int check_shutdown(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int running = shm->simulation_running;
    int station_closed = shm->station_closed;
    sem_unlock(SEM_SHM_MUTEX);
    
    return !running || station_closed;
}

/*============================================================================
 * MAIN FUNCTION
 *============================================================================*/

int main(int argc, char *argv[]) {
    /* Parse office ID from command line argument */
    if (argc > 1) {
        g_office_id = atoi(argv[1]);
    }
    
    /* Check log mode - only print to stdout if not minimal */
    const char *log_mode = getenv("BUS_LOG_MODE");
    int is_minimal = (log_mode && strcmp(log_mode, "minimal") == 0);
    
    if (!is_minimal) {
        printf("[TICKET_OFFICE %d] Starting (PID=%d)\n", g_office_id, getpid());
        fflush(stdout);
    }
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Attach to existing IPC resources */
    if (ipc_attach_all() != 0) {
        fprintf(stderr, "[TICKET_OFFICE %d] Failed to attach to IPC resources\n", g_office_id);
        exit(EXIT_FAILURE);
    }
    
    /* Get shared memory pointer */
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "[TICKET_OFFICE %d] Failed to get shared memory\n", g_office_id);
        exit(EXIT_FAILURE);
    }
    
    /* Register this ticket office */
    sem_lock(SEM_SHM_MUTEX);
    shm->ticket_office_pids[g_office_id] = getpid();
    sem_unlock(SEM_SHM_MUTEX);
    
    log_ticket_office(LOG_INFO, "Office %d started (PID=%d)", g_office_id, getpid());
    
    /* Validate office ID */
    if (g_office_id < 0 || g_office_id >= TICKET_OFFICES) {
        fprintf(stderr, "[TICKET_OFFICE %d] Invalid office ID (must be 0-%d)\n", 
                g_office_id, TICKET_OFFICES - 1);
        ipc_detach_all();
        exit(EXIT_FAILURE);
    }
    
    /* Select the appropriate semaphore for this office */
    int office_sem = SEM_TICKET_OFFICE(g_office_id);
    
    /* Main ticket processing loop */
    while (g_running) {
        /* Check for shutdown */
        if (check_shutdown(shm)) {
            log_ticket_office(LOG_INFO, "Office %d: Shutdown signal received", g_office_id);
            break;
        }
        
        /* Use alarm to periodically interrupt blocking msgrcv for shutdown check */
        alarm(1);
        
        /* Wait for a ticket request (blocking, interrupted by SIGALRM) */
        ticket_msg_t request;
        ssize_t ret = msg_recv_ticket(&request, MSG_TICKET_REQUEST, 0);
        
        alarm(0);  /* Cancel alarm */
        
        if (ret == -1) {
            if (errno == EINTR) {
                /* Interrupted by signal - check if we should continue */
                continue;
            }
            if (errno == EIDRM) {
                /* Message queue was removed - shutdown */
                break;
            }
            /* Other error - continue */
            continue;
        }

        /* Backpressure: a request has been removed from the ticket queue */
        sem_unlock(SEM_TICKET_QUEUE_SLOTS);
        
        /* Got a ticket request */
        log_ticket_office(LOG_INFO, "Office %d: Processing request from passenger PID %d",
                         g_office_id, request.passenger.pid);
        
        /* Mark office as busy */
        sem_lock(SEM_SHM_MUTEX);
        shm->ticket_office_busy[g_office_id] = request.passenger.pid;
        sem_unlock(SEM_SHM_MUTEX);
        
        /* Lock the office semaphore to ensure exclusive processing */
        sem_lock(office_sem);
        
        /* Process the request */
        process_ticket_request(shm, &request);
        
        /* Release office semaphore */
        sem_unlock(office_sem);
        
        /* Mark office as free */
        sem_lock(SEM_SHM_MUTEX);
        shm->ticket_office_busy[g_office_id] = 0;
        sem_unlock(SEM_SHM_MUTEX);
    }
    
    /* Drain remaining requests and send denial responses so passengers don't hang */
    log_ticket_office(LOG_INFO, "Office %d: Draining remaining requests", g_office_id);
    {
        ticket_msg_t request;
        while (msg_recv_ticket(&request, MSG_TICKET_REQUEST, IPC_NOWAIT) > 0) {
            sem_unlock(SEM_TICKET_QUEUE_SLOTS);  /* Backpressure */
            
            /* Send denial response */
            ticket_msg_t response;
            memset(&response, 0, sizeof(response));
            response.mtype = request.passenger.pid;
            response.passenger = request.passenger;
            response.ticket_office_id = g_office_id;
            response.approved = false;
            msg_send_ticket(&response);
            
            /* Update stats */
            sem_lock(SEM_SHM_MUTEX);
            shm->tickets_denied++;
            shm->passengers_in_office--;
            sem_unlock(SEM_SHM_MUTEX);
            
            log_ticket_office(LOG_INFO, "Office %d: Denied ticket to PID %d (station closed)",
                             g_office_id, request.passenger.pid);
        }
    }
    
    /* Cleanup */
    log_ticket_office(LOG_INFO, "Office %d shutting down", g_office_id);
    
    sem_lock(SEM_SHM_MUTEX);
    shm->ticket_office_pids[g_office_id] = 0;
    sem_unlock(SEM_SHM_MUTEX);
    
    ipc_detach_all();
    
    /* Use the same log_mode and is_minimal variables defined at the start of main() */
    if (!is_minimal) {
        printf("[TICKET_OFFICE %d] Terminated\n", g_office_id);
    }
    return 0;
}
