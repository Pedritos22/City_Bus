cmake_minimum_required(VERSION 3.10)
project(City_Bus C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")

# Default to Debug build if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

include_directories(include)

# Common source files (IPC and logging)
set(SRC_COMMON
    src/ipc.c
    src/logging.c
)

add_executable(main
    src/main.c
    ${SRC_COMMON}
)

add_executable(dispatcher
    src/dispatcher.c
    ${SRC_COMMON}
)

add_executable(driver
    src/driver.c
    ${SRC_COMMON}
)

add_executable(ticket_office
    src/ticket_office.c
    ${SRC_COMMON}
)

add_executable(passenger
    src/passenger.c
    ${SRC_COMMON}
)

# Link pthread for passenger (children are implemented as threads)
find_package(Threads REQUIRED)
target_link_libraries(passenger Threads::Threads)

# Create logs directory in build folder
add_custom_command(
    TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/logs
    COMMENT "Creating logs directory"
)