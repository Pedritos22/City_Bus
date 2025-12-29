cmake_minimum_required(VERSION 3.10)
project(City_Bus)


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)


include_directories(include)


set(SRC_COMMON
    src/ipc.c
    src/logging.c
)


add_executable(main src/main.c ${SRC_COMMON})
add_executable(dispatcher src/dispatcher.c ${SRC_COMMON})
add_executable(driver src/driver.c ${SRC_COMMON})
add_executable(ticket_office src/ticket_office.c ${SRC_COMMON})
add_executable(passenger src/passenger.c ${SRC_COMMON})

