cmake_minimum_required(VERSION 3.16)
project(umbra-v0 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable warnings to catch issues
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(ASMJIT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit)
file(GLOB ASMJIT_CORE_SRCS ${ASMJIT_ROOT}/asmjit/core/*.cpp)
file(GLOB ASMJIT_X86_SRCS  ${ASMJIT_ROOT}/asmjit/x86/*.cpp)
file(GLOB ASMJIT_ARM_SRCS  ${ASMJIT_ROOT}/asmjit/arm/*.cpp)
file(GLOB ASMJIT_SUP_SRCS  ${ASMJIT_ROOT}/asmjit/support/*.cpp)

set(CORE_SRCS
    src/ir/umbra_ir.cpp
    src/operators/scan_translator.cpp
    src/operators/select_translator.cpp
    src/operators/hash_join_translator.cpp
    src/backend/flying_start.cpp
    src/sql/sql_lexer.cpp
    src/sql/sql_parser.cpp
    src/sql/sql_catalog.cpp
    src/sql/sql_planner.cpp
    src/sql/sql_runner.cpp
)

set(ASMJIT_ALL_SRCS
    ${ASMJIT_CORE_SRCS}
    ${ASMJIT_X86_SRCS}
    ${ASMJIT_ARM_SRCS}
    ${ASMJIT_SUP_SRCS}
)

add_executable(umbra
    src/main.cpp
    ${CORE_SRCS}
    ${ASMJIT_ALL_SRCS}
)

target_compile_definitions(umbra PRIVATE ASMJIT_STATIC)
target_include_directories(umbra PRIVATE src/ ${ASMJIT_ROOT})

set_target_properties(umbra PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin
    OUTPUT_NAME umbra
)

add_executable(umbra_tests
    tests/test_runner.cpp
    ${CORE_SRCS}
    ${ASMJIT_ALL_SRCS}
)
target_compile_definitions(umbra_tests PRIVATE ASMJIT_STATIC)
target_include_directories(umbra_tests PRIVATE src/ ${ASMJIT_ROOT})
set_target_properties(umbra_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin
    OUTPUT_NAME umbra_tests
)
