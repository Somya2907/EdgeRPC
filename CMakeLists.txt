cmake_minimum_required(VERSION 3.20)
project(EdgeRPC VERSION 0.1.0 LANGUAGES CXX)

if(NOT UNIX)
    message(FATAL_ERROR "EdgeRPC requires POSIX Unix domain sockets (macOS or Linux).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(EDGERPC_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)

find_package(Threads REQUIRED)
add_library(edgerpc
    src/protocol.cpp
    src/transport_unix.cpp
    src/client.cpp
    src/server.cpp
    src/mock_executor.cpp)
target_include_directories(edgerpc PUBLIC include)
target_link_libraries(edgerpc PUBLIC Threads::Threads)
target_compile_options(edgerpc PRIVATE -Wall -Wextra -Wpedantic)
if(EDGERPC_SANITIZERS)
    target_compile_options(edgerpc PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(edgerpc PUBLIC -fsanitize=address,undefined)
endif()

add_executable(edgerpc_server apps/edgerpc_server.cpp)
add_executable(edgerpc_demo apps/edgerpc_demo.cpp)
foreach(app edgerpc_server edgerpc_demo)
    target_link_libraries(${app} PRIVATE edgerpc)
    target_include_directories(${app} PRIVATE apps)
    target_compile_options(${app} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()
