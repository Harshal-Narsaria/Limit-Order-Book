cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Benchmarks are reported at -O2 by default (CMake's Release default is -O3).
# Turn on LOB_NATIVE to compare against -O3 -march=native.
option(LOB_NATIVE "Build with -O3 -march=native" OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(LOB_NATIVE)
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
    endif()
endif()

function(lob_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endfunction()

# --- Library -----------------------------------------------------------------
add_library(lob
    src/map_order_book.cpp
    src/flat_order_book.cpp
    src/message_generator.cpp
)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
lob_warnings(lob)

# --- Tests -------------------------------------------------------------------
enable_testing()

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE lob)
lob_warnings(order_book_tests)
add_test(NAME order_book_tests COMMAND order_book_tests)

add_executable(differential_test tests/differential_test.cpp)
target_link_libraries(differential_test PRIVATE lob)
lob_warnings(differential_test)
add_test(NAME differential_test COMMAND differential_test)

# --- Benchmark ---------------------------------------------------------------
add_executable(lob_benchmark bench/benchmark.cpp)
target_link_libraries(lob_benchmark PRIVATE lob)
target_compile_definitions(lob_benchmark PRIVATE LOB_BUILD_FLAGS="${CMAKE_CXX_FLAGS_RELEASE}")
lob_warnings(lob_benchmark)
