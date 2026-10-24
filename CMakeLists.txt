cmake_minimum_required(VERSION 3.20)
project(OrderBook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ORDERBOOK_NATIVE   "Compile with -march=native"                      ON)
option(ORDERBOOK_SANITIZE "Build with AddressSanitizer + UBSan (tests only)" OFF)
option(ORDERBOOK_BENCH    "Build Google Benchmark suite (fetches gbench)"    ON)

find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
  if(ORDERBOOK_NATIVE)
    add_compile_options(-march=native)
  endif()
endif()
# Keep frame pointers so `perf record -g` gives usable call stacks.
add_compile_options(-fno-omit-frame-pointer)

if(ORDERBOOK_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------- libraries
add_library(orderbook_engine INTERFACE)
target_include_directories(orderbook_engine INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
  ${CMAKE_CURRENT_SOURCE_DIR}/common/include)
target_link_libraries(orderbook_engine INTERFACE Threads::Threads)

add_library(orderbook_reference INTERFACE)
target_include_directories(orderbook_reference INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/reference/include)
target_link_libraries(orderbook_reference INTERFACE orderbook_engine)

# ------------------------------------------------------------------ targets
# Each target is added only if its source exists, so every stage of the
# project (and every commit in its history) configures and builds cleanly.
enable_testing()

function(ob_exe name src lib)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ${lib})
  endif()
endfunction()

ob_exe(unit_tests         tests/unit_tests.cpp      orderbook_engine)
ob_exe(fuzz_diff          tests/fuzz_diff.cpp       orderbook_reference)
ob_exe(orderbook_gen      replay/gen_main.cpp       orderbook_engine)
ob_exe(orderbook_convert  replay/convert_main.cpp   orderbook_engine)
ob_exe(orderbook_replay   replay/replay_main.cpp    orderbook_engine)
ob_exe(orderbook_pipeline replay/pipeline_main.cpp  orderbook_engine)
ob_exe(orderbook_backtest strategy/backtest_main.cpp orderbook_engine)

if(TARGET unit_tests)
  add_test(NAME unit_tests COMMAND unit_tests)
endif()
if(TARGET fuzz_diff)
  add_test(NAME fuzz_diff_quick COMMAND fuzz_diff 30 20000)
endif()

# --------------------------------------------------------------- benchmarks
if(ORDERBOOK_BENCH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_engine.cpp)
  find_package(benchmark QUIET)
  if(NOT benchmark_FOUND)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googlebenchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG v1.8.3
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googlebenchmark)
  endif()
  add_executable(bench_engine bench/bench_engine.cpp)
  target_link_libraries(bench_engine PRIVATE orderbook_reference benchmark::benchmark)
endif()
