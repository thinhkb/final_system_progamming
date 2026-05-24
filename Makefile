CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS := -pthread

SRC := src/main.c
OBJ := $(SRC:.c=.o)
BIN := httpd

.PHONY: all clean test test-unit test-integration bench

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: test-unit test-integration

test-unit:
	@echo "unit tests will be added in later tasks"

test-integration:
	@echo "integration tests will be added in later tasks"

bench:
	@echo "benchmark will be added in later tasks"

clean:
	rm -f $(BIN) src/*.o tests/*.o tests/unit_http tests/unit_files tests/unit_thread_pool access.log
