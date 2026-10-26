CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -Iinclude

SOURCES := src/page.cpp src/heap_file.cpp src/buffer_pool.cpp src/record_store.cpp src/tuple.cpp src/catalog.cpp src/table_store.cpp src/query.cpp src/sql_lexer.cpp src/sql_parser.cpp src/sql_executor.cpp src/bplus_tree.cpp
HEADERS := include/pageforge/page.hpp include/pageforge/heap_file.hpp include/pageforge/buffer_pool.hpp include/pageforge/record_store.hpp include/pageforge/tuple.hpp include/pageforge/catalog.hpp include/pageforge/table_store.hpp include/pageforge/query.hpp include/pageforge/sql_lexer.hpp include/pageforge/sql_parser.hpp include/pageforge/sql_executor.hpp include/pageforge/bplus_tree.hpp
TEST_BINARY := build/pageforge_tests
CLI_BINARY := build/pageforge
BENCH_BINARY := build/pageforge_bench

.PHONY: all test demo-smoke benchmark benchmark-smoke benchmark-environment check clean

all: $(TEST_BINARY) $(CLI_BINARY) $(BENCH_BINARY)

$(TEST_BINARY): $(SOURCES) tests/test_main.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) tests/test_main.cpp -o $(TEST_BINARY)

$(CLI_BINARY): $(SOURCES) src/cli.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) src/cli.cpp -o $(CLI_BINARY)

$(BENCH_BINARY): $(SOURCES) src/benchmark.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) src/benchmark.cpp -o $(BENCH_BINARY)

test: $(TEST_BINARY) $(CLI_BINARY)
	./$(TEST_BINARY)
	sh tests/cli_smoke.sh ./$(CLI_BINARY)

demo-smoke: $(CLI_BINARY)
	sh tests/demo_database_smoke.sh ./$(CLI_BINARY) ./tools/create_demo_database.sh

benchmark: $(BENCH_BINARY)

benchmark-smoke: $(BENCH_BINARY)
	python3 tests/benchmark_smoke.py ./$(BENCH_BINARY)

benchmark-environment:
	@python3 ./tools/environment_report.py --compiler="$(CXX)" --cppflags="$(CPPFLAGS)" --cxxflags="$(CXXFLAGS)"

check: test demo-smoke benchmark-smoke

clean:
	rm -rf build
