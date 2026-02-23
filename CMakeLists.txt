set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_minimum_required(VERSION 3.10)
project(lobook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add flags for performance and warnings
add_compile_options(-O3 -Wall -Wextra -Wpedantic)

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
