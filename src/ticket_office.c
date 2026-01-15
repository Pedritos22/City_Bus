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
static int g_office_id = 0;  /**< This ticket office's ID */


/**
 * Handler for SIGTERM/SIGINT - shutdown signal.
 */
static void handle_shutdown(int sig) {
    (void)sig;
    g_running = 0;
}

/**
 * Setup signal handlers.
 * Uses sigaction() for reliable signal handling.
 */
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
}


/**
 * Validate passenger data.
 * Ensures passenger information is reasonable.
 */
static int validate_passenger(const passenger_info_t *passenger) {
    // Validate age
    if (passenger->age < MIN_AGE || passenger->age > MAX_AGE) {
        return 0;
    }
    
    // Validate PID
    if (passenger->pid <= 0) {
        return 0;
    }
    
    return 1;
}

/**
 * Process a single ticket request.
 * Registers the passenger and issues a ticket.
 */
static void process_ticket_request(shm_data_t *shm, ticket_msg_t *request) {
    ticket_msg_t response;
    memset(&response, 0, sizeof(response));
    
    // Set response mtype to passenger's PID
    response.mtype = request->passenger.pid;
    response.passenger = request->passenger;
    response.ticket_office_id = g_office_id;
    
    // Validate passenger data
    if (!validate_passenger(&request->passenger)) {
        response.approved = false;
        log_ticket_office(LOG_WARN, "Office %d: Invalid passenger data from PID %d",
                         g_office_id, request->passenger.pid);
    } else {
        // Simulate ticket processing time TODO: CHECK IF AIGHT WITH PROWADZACY
        sleep(TICKET_PROCESS_TIME);
        
        // Issue the ticket
        response.approved = true;
        response.passenger.has_ticket = true;
        
        // Update shared memory
        sem_lock(SEM_SHM_MUTEX);
        shm->tickets_issued++;
        shm->passengers_in_office--;
        sem_unlock(SEM_SHM_MUTEX);
        
        // Log ticket with child info if works
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
    
    // Send response back to passenger
    if (msg_send_ticket(&response) == -1) {
        log_ticket_office(LOG_ERROR, "Office %d: Failed to send ticket response to PID %d",
                         g_office_id, request->passenger.pid);
    }
}

/**
 * Check for shutdown via shared memory or dispatcher message.
 */
static int check_shutdown(shm_data_t *shm) {
    sem_lock(SEM_SHM_MUTEX);
    int running = shm->simulation_running;
    sem_unlock(SEM_SHM_MUTEX);
    
    if (!running) {
        return 1;
    }
    
    // Check for dispatcher shutdown message (target for non-blocking)
    dispatch_msg_t dmsg;
    if (msg_recv_dispatch(&dmsg, MSG_DISPATCH_SHUTDOWN, IPC_NOWAIT) > 0) {
        return 1;
    }
    
    return 0;
}


int main(int argc, char *argv[]) {
    // Parse office ID from command line argument
    if (argc > 1) {
        g_office_id = atoi(argv[1]);
    }
    
    printf("[TICKET_OFFICE %d] Starting (PID=%d)\n", g_office_id, getpid());
    fflush(stdout);
    
    // Setup signal handlers
    setup_signals();
    
    // Attach to existing IPC resources
    if (ipc_attach_all() != 0) {
        fprintf(stderr, "[TICKET_OFFICE %d] Failed to attach to IPC resources\n", g_office_id);
        exit(EXIT_FAILURE);
    }
    
    shm_data_t *shm = ipc_get_shm();
    if (shm == NULL) {
        fprintf(stderr, "[TICKET_OFFICE %d] Failed to get shared memory\n", g_office_id);
        exit(EXIT_FAILURE);
    }
    
    // Register this ticket office
    sem_lock(SEM_SHM_MUTEX);
    shm->ticket_office_pids[g_office_id] = getpid();
    sem_unlock(SEM_SHM_MUTEX);
    
    log_ticket_office(LOG_INFO, "Office %d started (PID=%d)", g_office_id, getpid());
    
    // Select the semaphore for this office
    int office_sem = (g_office_id == 0) ? SEM_TICKET_OFFICE_0 : SEM_TICKET_OFFICE_1;
    
    // Main ticket processing
    while (g_running) {
        // Check for shutdown
        if (check_shutdown(shm)) {
            log_ticket_office(LOG_INFO, "Office %d: Shutdown signal received", g_office_id);
            break;
        }
        
        // Wait for a ticket request (blocking)
        ticket_msg_t request;
        ssize_t ret = msg_recv_ticket(&request, MSG_TICKET_REQUEST, 0);
        
        if (ret == -1) {
            if (errno == EINTR) {
                // Interrupted by signal, check if we should continue
                continue;
            }
            if (errno == EIDRM) {
                // Message queue was removed, SHUTDOWN
                break;
            }
            // Other error, continue
            continue;
        }
        
        // Got a ticket request
        log_ticket_office(LOG_INFO, "Office %d: Processing request from passenger PID %d",
                         g_office_id, request.passenger.pid);
        
        // Mark office as busy
        sem_lock(SEM_SHM_MUTEX);
        shm->ticket_office_busy[g_office_id] = request.passenger.pid;
        sem_unlock(SEM_SHM_MUTEX);
        
        sem_lock(office_sem);
        
        // Process the request
        process_ticket_request(shm, &request);
        
        // Release office semaphore
        sem_unlock(office_sem);
        
        // Mark office as FINALLY FREE
        sem_lock(SEM_SHM_MUTEX);
        shm->ticket_office_busy[g_office_id] = 0;
        sem_unlock(SEM_SHM_MUTEX);
    }
    
    // Cleanup
    log_ticket_office(LOG_INFO, "Office %d shutting down", g_office_id);
    
    sem_lock(SEM_SHM_MUTEX);
    shm->ticket_office_pids[g_office_id] = 0;
    sem_unlock(SEM_SHM_MUTEX);
    
    ipc_detach_all();
    
    printf("[TICKET_OFFICE %d] Terminated\n", g_office_id);
    return 0;
}
