CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -Iinclude

SOURCES := src/page.cpp src/heap_file.cpp src/buffer_pool.cpp
TEST_BINARY := build/pageforge_tests

.PHONY: all test check clean

all: $(TEST_BINARY)

$(TEST_BINARY): $(SOURCES) tests/test_main.cpp include/pageforge/page.hpp include/pageforge/heap_file.hpp include/pageforge/buffer_pool.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) tests/test_main.cpp -o $(TEST_BINARY)

test: $(TEST_BINARY)
	./$(TEST_BINARY)

check: test

clean:
	rm -rf build
