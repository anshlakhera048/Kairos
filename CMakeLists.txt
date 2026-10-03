cmake_minimum_required(VERSION 3.24)

project(Kairos
    VERSION 0.1.0
    DESCRIPTION "Low-latency C++20 matching engine, realistic backtester, market-making arena"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings for OUR targets only (never leak -Werror into FetchContent deps).
add_library(kairos_warnings INTERFACE)
target_compile_options(kairos_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

# Header-only core interface: include/kairos
add_library(kairos INTERFACE)
add_library(kairos::kairos ALIAS kairos)
target_include_directories(kairos INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(kairos INTERFACE kairos_warnings)

# Compiled core
add_library(kairos_core STATIC
    src/order_book.cpp
    src/engine.cpp
    src/data_format.cpp
    src/l2_book.cpp
    src/account.cpp
)
target_link_libraries(kairos_core PUBLIC kairos)

option(KAIROS_BUILD_TESTS "Build unit/differential/randomized tests" ON)
option(KAIROS_BUILD_BENCHMARKS "Build benchmarks (never run in CI)" OFF)

include(FetchContent)

if(KAIROS_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KAIROS_BUILD_BENCHMARKS)
    FetchContent_Declare(googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
    add_subdirectory(bench)
endif()

add_subdirectory(tools)
