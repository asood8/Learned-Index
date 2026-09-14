# Every program here is one .cpp file under cpp/, so each one could be
# compiled directly. This just saves typing the commands, and puts the
# binaries in build/ instead of next to the source.
# Needs g++ with C++17, and python3 for difftest and data.
#
#   make test        build and run the three test suites
#   make asan        the same suites under AddressSanitizer + UBSan
#   make demos       the durability and catalog restart demos
#   make difftest    compare against SQLite (RUNS=200 by default)
#   make data        generate the benchmark datasets (needs numpy)
#   make all         build every program, benchmarks included
#   make repl        build and start the SQL shell
#   make clean

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
SANFLAGS := -O1 -g -std=c++17 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
RUNS     ?= 200

BUILD    := build
HEADERS  := $(wildcard cpp/*.h) $(wildcard cpp/third_party/pgm/*.hpp)
PROGRAMS := $(patsubst cpp/%.cpp,$(BUILD)/%,$(wildcard cpp/*.cpp))
TESTS    := test_suite sql_parser_test executor_test

.PHONY: all test asan demos difftest data repl clean

all: $(PROGRAMS)

$(BUILD)/%: cpp/%.cpp $(HEADERS) | $(BUILD) results
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/asan/%: cpp/%.cpp $(HEADERS) | $(BUILD)/asan results
	$(CXX) $(SANFLAGS) -o $@ $<

$(BUILD) $(BUILD)/asan results:
	mkdir -p $@

test: $(addprefix $(BUILD)/,$(TESTS))
	./$(BUILD)/test_suite
	./$(BUILD)/sql_parser_test
	./$(BUILD)/executor_test

# The B+-tree baseline deliberately has no destructor (it's only used
# by benchmarks), so leak checking is off for the one suite that builds
# it. Everything else runs with the default checks.
asan: $(addprefix $(BUILD)/asan/,$(TESTS))
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/asan/test_suite
	./$(BUILD)/asan/sql_parser_test
	./$(BUILD)/asan/executor_test

demos: $(BUILD)/durability_demo $(BUILD)/phase8_demo
	./$(BUILD)/durability_demo
	./$(BUILD)/phase8_demo

difftest: $(BUILD)/sql_harness
	python3 python/sqlite_diff_test.py --harness $(BUILD)/sql_harness --runs $(RUNS)

data:
	python3 python/data_gen.py --n 1000000 --outdir data

repl: $(BUILD)/repl
	./$(BUILD)/repl

clean:
	rm -rf $(BUILD)
