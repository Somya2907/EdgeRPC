cmake_minimum_required(VERSION 3.20)
project(EdgeRPC VERSION 0.3.0 LANGUAGES CXX)

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
    src/thread_pool.cpp
    src/shared_buffer_posix.cpp
    src/mock_executor.cpp
    src/metrics.cpp)
target_include_directories(edgerpc PUBLIC include)
target_link_libraries(edgerpc PUBLIC Threads::Threads)
target_compile_options(edgerpc PRIVATE -Wall -Wextra -Wpedantic)
if(EDGERPC_SANITIZERS)
    target_compile_options(edgerpc PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(edgerpc PUBLIC -fsanitize=address,undefined)
endif()

add_executable(edgerpc_server apps/edgerpc_server.cpp)
add_executable(edgerpc_demo apps/edgerpc_demo.cpp)
add_executable(edgerpc_benchmark benchmarks/benchmark.cpp)
foreach(app edgerpc_server edgerpc_demo edgerpc_benchmark)
    target_link_libraries(${app} PRIVATE edgerpc)
    target_include_directories(${app} PRIVATE apps)
    target_compile_options(${app} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE edgerpc)
    target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME protocol COMMAND test_protocol)
    add_executable(test_async tests/test_async.cpp)
    target_link_libraries(test_async PRIVATE edgerpc)
    target_compile_options(test_async PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME async COMMAND test_async)
    add_executable(test_shared_memory tests/test_shared_memory.cpp)
    target_link_libraries(test_shared_memory PRIVATE edgerpc)
    target_compile_options(test_shared_memory PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME shared_memory COMMAND test_shared_memory)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(NAME integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.py
        --bin-dir $<TARGET_FILE_DIR:edgerpc_server>)
    set_tests_properties(protocol PROPERTIES TIMEOUT 20)
    set_tests_properties(async PROPERTIES TIMEOUT 40)
    set_tests_properties(shared_memory PROPERTIES TIMEOUT 20)
    set_tests_properties(integration PROPERTIES TIMEOUT 90)
endif()
