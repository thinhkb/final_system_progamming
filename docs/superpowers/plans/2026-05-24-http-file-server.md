# HTTP File Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the full A3 final-project submission: a C11/POSIX multi-threaded HTTP file server with source code, tests, benchmark tooling, README, and report draft.

**Architecture:** The server uses one acceptor loop and a fixed pool of worker threads connected by a bounded producer-consumer socket queue. HTTP parsing, filesystem access, logging, server lifecycle, and thread-pool behavior are separate modules with narrow interfaces.

**Tech Stack:** C11, POSIX sockets, pthreads, GCC, GNU Make, shell scripts, curl, standard Unix tools.

---

## File Structure

- Create `Makefile`: build targets `all`, `clean`, `test`, `test-unit`, `test-integration`, and `bench`.
- Create `include/config.h`: server configuration defaults and struct.
- Create `include/http.h`, `src/http.c`: request parsing, status text, keep-alive decisions, response header helpers.
- Create `include/files.h`, `src/files.c`: MIME lookup, safe path resolution, file metadata, directory listing HTML.
- Create `include/thread_pool.h`, `src/thread_pool.c`: bounded queue, fixed workers, enqueue/dequeue/shutdown.
- Create `include/server.h`, `src/server.c`: listening socket, accept loop, worker connection handling.
- Create `include/log.h`, `src/log.c`: Common Log Format access logging.
- Create `src/main.c`: CLI parsing, signal handling, configuration, server startup.
- Create `tests/unit_http.c`: parser and keep-alive unit tests.
- Create `tests/unit_files.c`: MIME and path-safety unit tests.
- Create `tests/unit_thread_pool.c`: bounded queue behavior tests.
- Create `tests/run_tests.sh`: black-box HTTP tests.
- Create `bench/bench.sh`: 100+ client benchmark.
- Create `www/index.html`, `www/about.txt`, `www/assets/style.css`, `www/listing/a.txt`, `www/listing/b.txt`: sample content.
- Create `README.md`: required project metadata and usage.
- Create `report/report.md`: required report draft with diagrams.

## Task 1: Scaffold Build System and Sample Content

**Files:**
- Create: `Makefile`
- Create: `include/config.h`
- Create: `src/main.c`
- Create: `www/index.html`
- Create: `www/about.txt`
- Create: `www/assets/style.css`
- Create: `www/listing/a.txt`
- Create: `www/listing/b.txt`

- [ ] **Step 1: Create directory layout**

Run:
```bash
rtk mkdir -p src include tests bench www/assets www/listing report
```

Expected: directories exist with no command errors.

- [ ] **Step 2: Create initial configuration header**

Create `include/config.h` with:
```c
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define DEFAULT_HOST "0.0.0.0"
#define DEFAULT_PORT 8080
#define DEFAULT_THREAD_COUNT 4
#define DEFAULT_QUEUE_CAPACITY 64
#define DEFAULT_DOC_ROOT "www"
#define DEFAULT_ACCESS_LOG "access.log"
#define READ_BUFFER_SIZE 8192
#define RESPONSE_BUFFER_SIZE 8192

typedef struct {
    const char *host;
    int port;
    int thread_count;
    int queue_capacity;
    const char *doc_root;
    const char *access_log;
} server_config_t;

#endif
```

- [ ] **Step 3: Create minimal `src/main.c`**

Create `src/main.c` with:
```c
#include "config.h"

#include <stdio.h>

int main(void) {
    printf("http-file-server scaffold ready on default port %d\n", DEFAULT_PORT);
    return 0;
}
```

- [ ] **Step 4: Create `Makefile`**

Create `Makefile` with:
```make
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
```

- [ ] **Step 5: Add sample web content**

Create `www/index.html` with a short HTML page containing `System Programming HTTP Server`.
Create `www/about.txt` with `static text file served by the final project server`.
Create `www/assets/style.css` with `body { font-family: sans-serif; }`.
Create `www/listing/a.txt` with `alpha`.
Create `www/listing/b.txt` with `beta`.

- [ ] **Step 6: Verify build**

Run:
```bash
rtk make clean
rtk make
./httpd
```

Expected:
```text
http-file-server scaffold ready on default port 8080
```

- [ ] **Step 7: Commit**

Run:
```bash
rtk git add Makefile include/config.h src/main.c www
rtk git commit -m "chore: scaffold final project build"
```

## Task 2: Implement HTTP Request Parsing

**Files:**
- Create: `include/http.h`
- Create: `src/http.c`
- Create: `tests/unit_http.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing HTTP parser tests**

Create `tests/unit_http.c` with tests for:
- valid `GET /index.html HTTP/1.1`
- valid `HEAD /about.txt HTTP/1.0`
- unsupported method `POST`
- malformed request line
- HTTP/1.1 keep-alive default
- HTTP/1.0 keep-alive only with `Connection: keep-alive`
- `Connection: close` disables keep-alive

Use this test shape:
```c
#include "http.h"

#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)
#define ASSERT_STR_EQ(a, b) do { if (strcmp((a), (b)) != 0) { fprintf(stderr, "FAIL: expected '%s' got '%s'\n", (b), (a)); return 1; } } while (0)

static int test_parse_get_http11(void) {
    http_request_t request;
    const char *raw = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(http_parse_request(raw, strlen(raw), &request) == HTTP_PARSE_OK);
    ASSERT_TRUE(request.method == HTTP_METHOD_GET);
    ASSERT_STR_EQ(request.path, "/index.html");
    ASSERT_TRUE(request.version == HTTP_VERSION_11);
    ASSERT_TRUE(http_should_keep_alive(&request));
    return 0;
}

int main(void) {
    ASSERT_TRUE(test_parse_get_http11() == 0);
    puts("unit_http: PASS");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
rtk make test-unit
```

Expected: compilation fails because `http.h` and parser functions do not exist.

- [ ] **Step 3: Implement parser interface**

Create `include/http.h` defining:
- `http_method_t` with `HTTP_METHOD_GET`, `HTTP_METHOD_HEAD`, `HTTP_METHOD_UNSUPPORTED`
- `http_version_t` with `HTTP_VERSION_10`, `HTTP_VERSION_11`, `HTTP_VERSION_UNKNOWN`
- `http_parse_result_t` with `HTTP_PARSE_OK`, `HTTP_PARSE_BAD_REQUEST`
- `http_request_t` containing fixed-size `method_text`, `path`, `version_text`, `connection`, method enum, version enum, and `keep_alive_requested`
- `http_parse_request`
- `http_should_keep_alive`
- `http_status_text`

- [ ] **Step 4: Implement parser logic**

Create `src/http.c` to:
- split request line on spaces
- reject missing method, path, or version
- copy bounded strings with null termination
- classify `GET`, `HEAD`, and unsupported methods
- classify `HTTP/1.0` and `HTTP/1.1`
- scan headers case-insensitively for `Connection`
- default HTTP/1.1 to keep-alive unless `Connection: close`
- default HTTP/1.0 to close unless `Connection: keep-alive`

- [ ] **Step 5: Update Makefile unit target**

Modify `Makefile` so:
```make
COMMON_SRC := src/http.c
TEST_HTTP_SRC := tests/unit_http.c $(COMMON_SRC)

tests/unit_http: $(TEST_HTTP_SRC)
	$(CC) $(CFLAGS) -o $@ $(TEST_HTTP_SRC) $(LDFLAGS)

test-unit: tests/unit_http
	./tests/unit_http
```

- [ ] **Step 6: Verify tests pass**

Run:
```bash
rtk make clean
rtk make test-unit
```

Expected:
```text
unit_http: PASS
```

- [ ] **Step 7: Commit**

Run:
```bash
rtk git add Makefile include/http.h src/http.c tests/unit_http.c
rtk git commit -m "feat: add HTTP request parser"
```

## Task 3: Implement Safe Filesystem Helpers

**Files:**
- Create: `include/files.h`
- Create: `src/files.c`
- Create: `tests/unit_files.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing filesystem tests**

Create `tests/unit_files.c` with tests for:
- `.html` maps to `text/html`
- `.css` maps to `text/css`
- unknown extension maps to `application/octet-stream`
- `/index.html` resolves under `www`
- `/../etc/passwd` is rejected
- `/listing/` is classified as a directory

Use assertions that call `file_mime_type`, `file_resolve_path`, and `file_stat_path`.

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
rtk make test-unit
```

Expected: compilation fails because `files.h` and filesystem helpers do not exist.

- [ ] **Step 3: Implement filesystem interface**

Create `include/files.h` defining:
- `file_result_t` with `FILE_RESULT_OK`, `FILE_RESULT_NOT_FOUND`, `FILE_RESULT_FORBIDDEN`, `FILE_RESULT_ERROR`
- `file_kind_t` with `FILE_KIND_REGULAR`, `FILE_KIND_DIRECTORY`
- `file_info_t` with resolved path, kind, size, and MIME type
- `file_mime_type`
- `file_resolve_path`
- `file_stat_path`
- `file_build_directory_listing`

- [ ] **Step 4: Implement safe path resolution**

Create `src/files.c` to:
- URL-decode `%20` and common escaped path bytes
- reject decoded paths containing `..`
- join doc root and request path
- call `realpath` on existing paths
- confirm the resolved path has the document root prefix
- use `stat` to distinguish regular files and directories

- [ ] **Step 5: Implement directory listing HTML**

In `src/files.c`, implement `file_build_directory_listing` using `opendir`, `readdir`, and `snprintf` into a caller-provided buffer. Include links for non-hidden entries, escape `<`, `>`, `&`, and `"`, and return `FILE_RESULT_OK` when the listing fits.

- [ ] **Step 6: Update Makefile unit target**

Add `src/files.c` to common unit sources and build `tests/unit_files`.

- [ ] **Step 7: Verify tests pass**

Run:
```bash
rtk make clean
rtk make test-unit
```

Expected:
```text
unit_http: PASS
unit_files: PASS
```

- [ ] **Step 8: Commit**

Run:
```bash
rtk git add Makefile include/files.h src/files.c tests/unit_files.c
rtk git commit -m "feat: add safe filesystem helpers"
```

## Task 4: Implement Thread Pool and Bounded Queue

**Files:**
- Create: `include/thread_pool.h`
- Create: `src/thread_pool.c`
- Create: `tests/unit_thread_pool.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing queue tests**

Create `tests/unit_thread_pool.c` with tests for:
- queue accepts values until capacity
- enqueue returns a failure code when full in nonblocking mode
- dequeue returns values FIFO
- shutdown wakes blocked consumers

Use small integer file-descriptor placeholders such as `10`, `11`, and `12`; do not open real sockets in this unit test.

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
rtk make test-unit
```

Expected: compilation fails because `thread_pool.h` and queue functions do not exist.

- [ ] **Step 3: Implement thread-pool interface**

Create `include/thread_pool.h` defining:
- opaque `socket_queue_t`
- `socket_queue_init`
- `socket_queue_destroy`
- `socket_queue_enqueue`
- `socket_queue_dequeue`
- `socket_queue_shutdown`
- `thread_pool_t`
- `thread_pool_start`
- `thread_pool_stop`

- [ ] **Step 4: Implement bounded queue**

Create `src/thread_pool.c` using:
- `pthread_mutex_t`
- `pthread_cond_t not_empty`
- `pthread_cond_t not_full`
- circular buffer of `int` sockets
- `capacity`, `head`, `tail`, `count`, and `shutdown` fields

Return explicit success/failure codes so the accept loop can close sockets when the queue is full or shutting down.

- [ ] **Step 5: Implement fixed worker lifecycle**

In `src/thread_pool.c`, allocate `pthread_t` array, start exactly the configured number of workers, and join them during stop. Workers call a function pointer with each accepted socket.

- [ ] **Step 6: Update Makefile**

Build `tests/unit_thread_pool` with `src/thread_pool.c`.

- [ ] **Step 7: Verify tests pass**

Run:
```bash
rtk make clean
rtk make test-unit
```

Expected:
```text
unit_http: PASS
unit_files: PASS
unit_thread_pool: PASS
```

- [ ] **Step 8: Commit**

Run:
```bash
rtk git add Makefile include/thread_pool.h src/thread_pool.c tests/unit_thread_pool.c
rtk git commit -m "feat: add bounded worker queue"
```

## Task 5: Implement Server Socket and Static File Responses

**Files:**
- Create: `include/server.h`
- Create: `src/server.c`
- Modify: `src/main.c`
- Modify: `Makefile`
- Create: `tests/run_tests.sh`

- [ ] **Step 1: Write failing integration tests**

Create `tests/run_tests.sh` that:
- builds the server
- starts `./httpd -p 18080 -r www -t 4 -q 16` in the background
- waits until `/index.html` responds
- checks `GET /index.html` returns status `200` and body contains `System Programming HTTP Server`
- checks `HEAD /about.txt` returns status `200` and an empty body
- checks `/missing.txt` returns `404`
- checks `/../Makefile` returns `403`
- checks `/assets/style.css` returns `Content-Type: text/css`
- kills the server and exits nonzero on the first failure

- [ ] **Step 2: Run integration tests to verify failure**

Run:
```bash
rtk make test-integration
```

Expected: test fails because `./httpd` does not yet accept CLI flags or listen on a socket.

- [ ] **Step 3: Implement server interface**

Create `include/server.h` defining:
- `server_t`
- `server_init`
- `server_run`
- `server_stop`
- `server_destroy`

- [ ] **Step 4: Implement listening socket**

Create `src/server.c` to:
- use `getaddrinfo`, `socket`, `setsockopt(SO_REUSEADDR)`, `bind`, and `listen`
- accept clients in a loop
- enqueue accepted sockets into the bounded queue
- close clients immediately when queue enqueue fails

- [ ] **Step 5: Implement connection worker**

In `src/server.c`, implement worker handling to:
- read from a socket
- parse one request at a time
- route to filesystem helpers
- send status line, headers, and body
- close when keep-alive is false

- [ ] **Step 6: Implement CLI parsing**

Modify `src/main.c` to support:
- `-h host`
- `-p port`
- `-r document_root`
- `-t thread_count`
- `-q queue_capacity`
- `-l access_log`

Print usage and exit nonzero on invalid values.

- [ ] **Step 7: Update Makefile application sources**

Set:
```make
SRC := src/main.c src/server.c src/thread_pool.c src/http.c src/files.c
```

- [ ] **Step 8: Verify integration tests pass**

Run:
```bash
rtk make clean
rtk make
rtk make test-integration
```

Expected: all scripted integration checks pass.

- [ ] **Step 9: Commit**

Run:
```bash
rtk git add Makefile include/server.h src/server.c src/main.c tests/run_tests.sh
rtk git commit -m "feat: serve static HTTP files"
```

## Task 6: Add Keep-Alive, Directory Listings, and Access Logging

**Files:**
- Create: `include/log.h`
- Create: `src/log.c`
- Modify: `src/server.c`
- Modify: `src/files.c`
- Modify: `tests/run_tests.sh`
- Modify: `Makefile`

- [ ] **Step 1: Extend failing integration tests**

Add tests to `tests/run_tests.sh` for:
- `GET /listing/` returns `200`, `text/html`, and links to `a.txt` and `b.txt`
- two HTTP/1.1 requests sent on one connection both receive responses
- unsupported method `POST /index.html HTTP/1.1` returns `501`
- `access.log` contains a Common Log Format-style line with `"GET /index.html HTTP/1.1" 200`

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
rtk make test-integration
```

Expected: at least the access-log check fails before `log.c` exists; keep-alive may also fail until the worker loop is updated.

- [ ] **Step 3: Implement logging interface**

Create `include/log.h` defining:
- `access_log_t`
- `access_log_open`
- `access_log_close`
- `access_log_write`

- [ ] **Step 4: Implement Common Log Format writes**

Create `src/log.c` to write lines shaped like:
```text
127.0.0.1 - - [24/May/2026:17:20:00 +0700] "GET /index.html HTTP/1.1" 200 128
```

Use a mutex around file writes so multiple worker threads do not interleave log lines.

- [ ] **Step 5: Wire logging into server**

Modify `src/server.c` so each completed request logs:
- client IP
- original request line
- status code
- body bytes sent

- [ ] **Step 6: Complete keep-alive loop**

Modify worker handling so a connection may process multiple requests until:
- parser returns bad request
- client sends `Connection: close`
- HTTP/1.0 request lacks `Connection: keep-alive`
- read returns EOF

- [ ] **Step 7: Update Makefile**

Add `src/log.c` to application sources.

- [ ] **Step 8: Verify integration tests pass**

Run:
```bash
rtk make clean
rtk make test
```

Expected: unit tests and integration tests pass.

- [ ] **Step 9: Commit**

Run:
```bash
rtk git add Makefile include/log.h src/log.c src/server.c src/files.c tests/run_tests.sh
rtk git commit -m "feat: add keep-alive listings and access logs"
```

## Task 7: Add Benchmark and Stress Test

**Files:**
- Create: `bench/bench.sh`
- Modify: `Makefile`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Add benchmark script**

Create `bench/bench.sh` to:
- accept optional host, port, path, and client count
- default to `127.0.0.1`, `18080`, `/index.html`, and `120`
- launch concurrent `curl` requests
- report total successes, failures, elapsed seconds, and requests per second
- exit nonzero when any request fails

- [ ] **Step 2: Add integration stress check**

Modify `tests/run_tests.sh` to run:
```bash
./bench/bench.sh 127.0.0.1 18080 /index.html 120
```

Expected: benchmark exits `0` while the test server is running.

- [ ] **Step 3: Update Makefile bench target**

Modify `Makefile` so:
```make
bench: all
	./bench/bench.sh
```

- [ ] **Step 4: Verify benchmark**

Run:
```bash
rtk make clean
rtk make
./httpd -p 18080 -r www -t 8 -q 128 -l access.log &
SERVER_PID=$!
sleep 1
./bench/bench.sh 127.0.0.1 18080 /index.html 120
kill $SERVER_PID
wait $SERVER_PID || true
```

Expected: benchmark reports 120 successes, 0 failures, and exits `0`.

- [ ] **Step 5: Verify full test suite**

Run:
```bash
rtk make test
```

Expected: all unit, integration, and stress checks pass.

- [ ] **Step 6: Commit**

Run:
```bash
rtk git add Makefile bench/bench.sh tests/run_tests.sh
rtk git commit -m "test: add concurrent benchmark"
```

## Task 8: Complete README and Technical Report

**Files:**
- Create: `README.md`
- Create: `report/report.md`
- Create: `report/diagrams.md`

- [ ] **Step 1: Write README**

Create `README.md` with:
- project title and short description
- group member placeholders using explicit fields: full name, student ID, email
- build instructions: `make`
- run example: `./httpd -p 8080 -r www -t 4 -q 64 -l access.log`
- test instructions: `make test`
- benchmark instructions: `make bench`
- usage examples with curl
- explanation of source tree
- individual contributions section with named areas of work

- [ ] **Step 2: Write diagrams**

Create `report/diagrams.md` with two Mermaid diagrams:
- architecture diagram showing acceptor, bounded queue, worker pool, HTTP parser, filesystem, logger
- request lifecycle diagram from client connection to response and log entry

- [ ] **Step 3: Write report draft**

Create `report/report.md` with assignment-required sections:
- Introduction
- Background & Theory
- System Design
- Implementation
- Testing & Validation
- Performance Analysis
- Conclusion & Future Work
- References

Include the architecture and request-flow diagrams from `report/diagrams.md`, describe fixed thread pools, producer-consumer queues, Keep-Alive, MIME type handling, safe path resolution, Common Log Format, and benchmark interpretation.

- [ ] **Step 4: Verify documentation references real commands**

Run:
```bash
rtk rg -n "make|httpd|bench|test" README.md report/report.md
```

Expected: README and report reference the implemented commands.

- [ ] **Step 5: Run final verification**

Run:
```bash
rtk make clean
rtk make
rtk make test
rtk make bench
```

Expected: all commands succeed.

- [ ] **Step 6: Commit**

Run:
```bash
rtk git add README.md report
rtk git commit -m "docs: add final project documentation"
```

## Task 9: Final Submission Polish

**Files:**
- Modify only files with defects found during final verification.

- [ ] **Step 1: Inspect final tree**

Run:
```bash
rtk find . -maxdepth 3 -type f | sort
```

Expected: source, include, tests, benchmark, `www`, README, report, and spec/plan files are present.

- [ ] **Step 2: Run compiler warning gate**

Run:
```bash
rtk make clean
rtk make
```

Expected: compile succeeds with `-Wall -Wextra -Werror -pedantic`.

- [ ] **Step 3: Run behavioral gate**

Run:
```bash
rtk make test
```

Expected: all tests pass clearly.

- [ ] **Step 4: Run benchmark gate**

Run:
```bash
rtk make bench
```

Expected: benchmark script completes with no failed requests.

- [ ] **Step 5: Check git status**

Run:
```bash
rtk git status --short
```

Expected: no uncommitted changes after final fixes are committed.

- [ ] **Step 6: Commit any final fixes**

If files changed during this task, run:
```bash
rtk git add <changed-files>
rtk git commit -m "fix: polish final project submission"
```

If no files changed, do not create an empty commit.

## Self-Review

- Spec coverage: The plan covers source code, Makefile, tests, benchmark, README, report, diagrams, fixed worker pool, bounded queue, Keep-Alive, MIME types, directory listings, Common Log Format, range-free static serving, and a 100+ client stress check.
- Placeholder scan: The plan intentionally avoids incomplete tasks, undefined future code, and unspecified commands.
- Type consistency: Module names, function names, and file paths are consistent across tasks.
- Scope check: The plan excludes TLS, CGI, uploads, and dynamic content to stay aligned with the A3 assignment.
