CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS := -pthread

SRC := src/main.c src/http.c src/files.c src/thread_pool.c src/server.c
COMMON_SRC := src/http.c src/files.c src/thread_pool.c src/server.c
TEST_HTTP_SRC := tests/unit_http.c $(COMMON_SRC)
TEST_FILES_SRC := tests/unit_files.c src/files.c
TEST_THREAD_POOL_SRC := tests/unit_thread_pool.c src/thread_pool.c
OBJ := $(SRC:.c=.o)
BIN := httpd

.PHONY: all clean test test-unit test-integration bench

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: test-unit test-integration

tests/unit_http: $(TEST_HTTP_SRC)
	$(CC) $(CFLAGS) -o $@ $(TEST_HTTP_SRC) $(LDFLAGS)

tests/unit_files: $(TEST_FILES_SRC)
	$(CC) $(CFLAGS) -o $@ $(TEST_FILES_SRC) $(LDFLAGS)

tests/unit_thread_pool: $(TEST_THREAD_POOL_SRC)
	$(CC) $(CFLAGS) -o $@ $(TEST_THREAD_POOL_SRC) $(LDFLAGS)

test-unit: tests/unit_http tests/unit_files tests/unit_thread_pool
	./tests/unit_http
	./tests/unit_files
	./tests/unit_thread_pool

test-integration:
	./tests/run_tests.sh

bench:
	@echo "benchmark will be added in later tasks"

clean:
	rm -f $(BIN) src/*.o tests/*.o tests/unit_http tests/unit_files tests/unit_thread_pool access.log
