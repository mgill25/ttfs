# Makefile for umbra-v0 — Tidy Tuples + Umbra IR + Flying Start (asmJIT)

CXX      = c++
BIN      = bin/umbra
TEST_BIN = bin/umbra_tests

# asmJIT is in third_party/asmjit/asmjit/ (the asmjit source tree)
# Include path: -Ithird_party/asmjit so that #include <asmjit/asmjit.h> resolves
ASMJIT_ROOT = third_party/asmjit
ASMJIT_DIR  = $(ASMJIT_ROOT)/asmjit

CXXFLAGS = -std=c++20 -Wall -Wextra -Wno-unused-parameter \
           -Isrc -I$(ASMJIT_ROOT) -DASMJIT_STATIC

# Release build: enable optimizations
CXXFLAGS_RELEASE = $(CXXFLAGS) -O2

# asmJIT sources (core + x86 + arm needed even on macOS since core references arm symbols)
ASMJIT_SRCS = $(wildcard $(ASMJIT_DIR)/core/*.cpp)    \
              $(wildcard $(ASMJIT_DIR)/x86/*.cpp)      \
              $(wildcard $(ASMJIT_DIR)/arm/*.cpp)      \
              $(wildcard $(ASMJIT_DIR)/support/*.cpp)

# Project sources
COMMON_SRCS = src/ir/umbra_ir.cpp \
              src/operators/scan_translator.cpp \
              src/operators/select_translator.cpp \
              src/operators/hash_join_translator.cpp \
              src/backend/flying_start.cpp \
              src/sql/sql_lexer.cpp \
              src/sql/sql_parser.cpp \
              src/sql/sql_catalog.cpp \
              src/sql/sql_planner.cpp \
              src/sql/sql_runner.cpp

APP_SRCS = src/main.cpp $(COMMON_SRCS) $(ASMJIT_SRCS)
TEST_SRCS = tests/test_runner.cpp $(COMMON_SRCS) $(ASMJIT_SRCS)

.PHONY: all run demo test clean

all: $(BIN)

$(BIN): $(APP_SRCS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $^

$(TEST_BIN): $(TEST_SRCS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $^

run: $(BIN)
	./$(BIN)

demo: $(BIN)
	./$(BIN) --demo

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf bin umbra-demo
