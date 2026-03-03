set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_minimum_required(VERSION 3.10)
project(lobook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add flags for performance and warnings
add_compile_options(-O3 -Wall -Wextra -Wpedantic -D_GNU_SOURCE)

include_directories(src)

add_executable(benchmark
    src/engine/simple_book.h
    src/engine/fast_book.h
    src/benchmark.cpp
)

add_executable(test_price_bitset
    src/engine/constants.h
    src/engine/price_bitset.h
    tests/test_price_bitset.cpp
)

add_executable(benchmark_sparse
    src/benchmark_sparse.cpp
    src/bench_fastbook_runner.cpp
    src/bench_bitsetbook_runner.cpp
)

add_executable(server
    src/server.cpp
)
target_link_libraries(server PRIVATE pthread)
