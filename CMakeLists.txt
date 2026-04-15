cmake_minimum_required(VERSION 3.10)
project(lobook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-O3 -Wall -Wextra -Wpedantic -D_GNU_SOURCE)

# Default: only the latency servers. Turn these ON when you need book benchmarks / tests.
option(LOBOOK_BUILD_BENCHMARKS "Build benchmark (Naive vs Fast book)" OFF)
option(LOBOOK_BUILD_BENCHMARK_SPARSE "Build benchmark_sparse (bitset vs array)" OFF)
option(LOBOOK_BUILD_TESTS "Build test executables" OFF)
option(LOBOOK_ENABLE_COVERAGE "Enable coverage instrumentation and report target" OFF)

if(LOBOOK_ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
  else()
    message(WARNING "LOBOOK_ENABLE_COVERAGE is ON, but compiler is not GCC/Clang; coverage flags were not applied.")
  endif()
endif()

# --- SPSC + matching engine (always built) ---
add_executable(server_cached src/server.cpp)
target_include_directories(server_cached PRIVATE src)
target_link_libraries(server_cached PRIVATE pthread)
target_compile_definitions(server_cached PRIVATE LOBOOK_SPSC_USE_CACHED_INDICES)

add_executable(server_nocached src/server.cpp)
target_include_directories(server_nocached PRIVATE src)
target_link_libraries(server_nocached PRIVATE pthread)

# Same binary as server_cached (backward-compatible name)
add_executable(server src/server.cpp)
target_include_directories(server PRIVATE src)
target_link_libraries(server PRIVATE pthread)
target_compile_definitions(server PRIVATE LOBOOK_SPSC_USE_CACHED_INDICES)

if(LOBOOK_BUILD_BENCHMARKS)
  add_executable(benchmark src/benchmark.cpp)
  target_include_directories(benchmark PRIVATE src)
endif()

if(LOBOOK_BUILD_BENCHMARK_SPARSE)
  add_executable(benchmark_sparse
    src/benchmark_sparse.cpp
    src/bench_fastbook_runner.cpp
    src/bench_bitsetbook_runner.cpp
  )
  target_include_directories(benchmark_sparse PRIVATE src)
endif()

if(LOBOOK_BUILD_TESTS)
  add_executable(test_price_bitset tests/test_price_bitset.cpp)
  target_include_directories(test_price_bitset PRIVATE src)

  add_executable(test_replace tests/test_replace.cpp)
  target_include_directories(test_replace PRIVATE src)

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
          --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
          --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
          --print-summary
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS test_price_bitset test_replace
        COMMENT "Running tests and generating coverage report"
      )
    else()
      message(WARNING "gcovr not found; coverage target will not be created.")
    endif()
  endif()
endif()
