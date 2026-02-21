cmake_minimum_required(VERSION 3.16)
project(umbra-v0 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

# Enable warnings to catch issues
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_executable(umbra-demo
    src/main.cpp
    src/ir/umbra_ir.cpp
    src/operators/scan_translator.cpp
    src/operators/select_translator.cpp
    src/operators/hash_join_translator.cpp
    src/backend/flying_start.cpp
)

target_include_directories(umbra-demo PRIVATE src/)
