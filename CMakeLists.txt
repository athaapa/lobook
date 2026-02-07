set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_minimum_required(VERSION 3.10)
project(lobook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add flags for performance and warnings
add_compile_options(-O3 -Wall -Wextra -Wpedantic)

include_directories(src)

add_executable(lobook_naive
    src/main.cpp
    src/naive/simple_book.h
)
