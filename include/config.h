#ifndef CONFIG_H
#define CONFIG_H

#define MAX_BUSES           3
#define BUS_CAPACITY        10
#define BIKE_CAPACITY       3
#define BOARDING_INTERVAL   8
#define MIN_RETURN_TIME     3
#define MAX_RETURN_TIME     8

#define TICKET_OFFICES      2
#define TICKET_PROCESS_TIME 1
#define MAX_TICKET_QUEUE_REQUESTS   200
#define MAX_BOARDING_QUEUE_REQUESTS 100

#define MAX_PASSENGERS      20
#define MIN_AGE             3
#define MAX_AGE             70
#define CHILD_AGE_LIMIT     8
#define VIP_PERCENT         1
#define BIKE_PERCENT        20
#define ADULT_WITH_CHILD_PERCENT  15
#define ADULT_MIN_AGE       18
#define MIN_ARRIVAL_MS      200
#define MAX_ARRIVAL_MS      1000

#define DISPATCHER_INTERVAL 3

#define LOG_DIR             "logs"
#define LOG_MASTER          "logs/master.log"
#define LOG_DISPATCHER      "logs/dispatcher.log"
#define LOG_TICKET_OFFICE   "logs/ticket_office.log"
#define LOG_DRIVER          "logs/driver.log"
#define LOG_PASSENGER       "logs/passenger.log"
#define LOG_STATS           "logs/stats.log"

#define IPC_KEY_BASE        0x4255
#define SHM_KEY             (IPC_KEY_BASE + 0x01)
#define SEM_KEY             (IPC_KEY_BASE + 0x02)
#define MSG_TICKET_KEY      (IPC_KEY_BASE + 0x03)
#define MSG_BOARDING_KEY    (IPC_KEY_BASE + 0x04)
#define MSG_DISPATCH_KEY    (IPC_KEY_BASE + 0x05)

#endif
