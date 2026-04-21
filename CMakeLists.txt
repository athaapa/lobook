cmake_minimum_required(VERSION 3.10)
project(lobook LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "lobook is Linux-only (uses perf_event_open, pthread_setaffinity_np, /proc, rdtsc).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-O3 -Wall -Wextra -Wpedantic -D_GNU_SOURCE)

option(LOBOOK_BUILD_BENCHMARKS "Build benchmark_book, benchmark_replace, benchmark_clock" OFF)
option(LOBOOK_BUILD_TESTS      "Build unit tests" OFF)
option(LOBOOK_ENABLE_COVERAGE  "Compile with -O0 + gcov instrumentation (DO NOT use for benchmarking)" OFF)

if(LOBOOK_ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "LOBOOK_ENABLE_COVERAGE=ON forces -O0; benchmark numbers will be ~4x worse.")
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
  else()
    message(WARNING "LOBOOK_ENABLE_COVERAGE=ON ignored: compiler is not GCC/Clang.")
  endif()
endif()

# ---------------------------------------------------------------------------
#  server  --  end-to-end latency harness
# ---------------------------------------------------------------------------
add_executable(server src/server.cpp)
target_include_directories(server PRIVATE src)
target_link_libraries(server PRIVATE pthread)

# ---------------------------------------------------------------------------
#  benchmarks
# ---------------------------------------------------------------------------
if(LOBOOK_BUILD_BENCHMARKS)
  add_executable(benchmark_book
    src/benchmark_book.cpp
    src/bench_runners/fastbook_runner.cpp
    src/bench_runners/bitsetbook_runner.cpp
  )
  target_include_directories(benchmark_book PRIVATE src)

  add_executable(benchmark_replace src/benchmark_replace.cpp)
  target_include_directories(benchmark_replace PRIVATE src)

  add_executable(benchmark_clock src/benchmark_clock.cpp)
  target_include_directories(benchmark_clock PRIVATE src)
endif()

# ---------------------------------------------------------------------------
#  tests
# ---------------------------------------------------------------------------
if(LOBOOK_BUILD_TESTS)
  enable_testing()

  foreach(t IN ITEMS test_price_bitset test_replace test_cancel test_match)
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE src)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  if(LOBOOK_ENABLE_COVERAGE)
    find_program(GCOVR_BIN gcovr)
    if(GCOVR_BIN)
      add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${GCOVR_BIN}
          -r ${CMAKE_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
          --exclude-directories tests
          --exclude ".*/fast_book.h"
          --exclude ".*/simple_book.h"
          --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
          --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
          --print-summary
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS test_price_bitset test_replace test_cancel test_match
        COMMENT "Running tests and generating coverage report"
      )
    else()
      message(WARNING "gcovr not found; coverage target will not be created.")
    endif()
  endif()
endif()
