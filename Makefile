CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -Iinclude

SOURCES := src/page.cpp src/heap_file.cpp src/buffer_pool.cpp src/record_store.cpp src/tuple.cpp src/catalog.cpp src/table_store.cpp
HEADERS := include/pageforge/page.hpp include/pageforge/heap_file.hpp include/pageforge/buffer_pool.hpp include/pageforge/record_store.hpp include/pageforge/tuple.hpp include/pageforge/catalog.hpp include/pageforge/table_store.hpp
TEST_BINARY := build/pageforge_tests
CLI_BINARY := build/pageforge

.PHONY: all test check clean

all: $(TEST_BINARY) $(CLI_BINARY)

$(TEST_BINARY): $(SOURCES) tests/test_main.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) tests/test_main.cpp -o $(TEST_BINARY)

$(CLI_BINARY): $(SOURCES) src/cli.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) src/cli.cpp -o $(CLI_BINARY)

test: $(TEST_BINARY) $(CLI_BINARY)
	./$(TEST_BINARY)
	sh tests/cli_smoke.sh ./$(CLI_BINARY)

check: test

clean:
	rm -rf build
