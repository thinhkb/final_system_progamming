# System Programming HTTP File Server

Final project for System Programming, option A3: HTTP file server.

This project implements a multi-threaded HTTP/1.0 and HTTP/1.1 static file server in C. It serves files from a document root, supports Keep-Alive, MIME types, directory listings, byte-range requests, Common Log Format access logs, and a fixed worker thread pool with a bounded producer-consumer queue.

## Group Members

> **TODO:** Replace the placeholder entries below with your real group member information before submission.

| Full name | Student ID | Email |
| --- | --- | --- |
| Member 1 | Student ID 1 | email1@example.com |
| Member 2 | Student ID 2 | email2@example.com |
| Member 3 | Student ID 3 | email3@example.com |

## Build

If you already have old build artifacts from another machine or OS, rebuild from scratch:

```bash
make clean
make
```

The build uses:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
```

## Run

```bash
./httpd -p 8080 -r www -t 4 -q 64 -l access.log
```

Options:

| Option | Meaning | Default |
| --- | --- | --- |
| `-h host` | Bind address | `0.0.0.0` |
| `-p port` | TCP port | `8080` |
| `-r doc_root` | Static file root | `www` |
| `-t threads` | Worker thread count | `4` |
| `-q queue` | Bounded queue capacity | `64` |
| `-l access_log` | Access log path | `access.log` |

## Usage Examples

```bash
curl http://127.0.0.1:8080/index.html
curl -I http://127.0.0.1:8080/about.txt
curl http://127.0.0.1:8080/listing/
curl -r 0-31 http://127.0.0.1:8080/about.txt
curl http://127.0.0.1:8080/index.html?cache=false
```

Unsupported methods return `501 Not Implemented`, missing files return `404 Not Found`, path traversal attempts return `403 Forbidden`, and unsatisfiable ranges return `416 Range Not Satisfiable`.

## Testing

Run all tests:

```bash
make test
```

The test suite includes:

- HTTP request parser unit tests
- filesystem and path-safety unit tests
- bounded queue unit tests
- black-box integration tests using `curl`
- byte-range response checks
- URL-encoded directory-listing links
- a 120-client concurrent stress check

## Benchmark

Run the standalone benchmark target:

```bash
make bench
```

The target starts the server on port `18080`, runs 120 concurrent requests against `/index.html`, reports successes, failures, elapsed time, and approximate requests per second, then stops the server.

The benchmark script can also be run against an already-running server:

```bash
bash ./bench/bench.sh 127.0.0.1 8080 /index.html 120
```

## Source Tree

| Path | Purpose |
| --- | --- |
| `src/main.c` | CLI parsing, signal handling, startup |
| `src/server.c` | listening socket, accept loop, client handling |
| `src/thread_pool.c` | fixed worker pool and bounded socket queue |
| `src/http.c` | HTTP request parsing and status text |
| `src/files.c` | MIME lookup, safe path resolution, directory listings |
| `src/log.c` | Common Log Format access logging |
| `include/` | public module headers |
| `tests/` | unit and integration tests |
| `bench/` | concurrent benchmark script |
| `www/` | sample document root |
| `report/` | technical report draft and diagrams |

## Individual Contributions

> **TODO:** Update this section to reflect actual work distribution among group members.

The project was structured as a modular implementation with the following areas:

| Area | Description |
|---|---|
| Server core | Listening socket setup, accept loop, client dispatch, signal handling |
| HTTP parsing | Request line and header parsing, method/version handling, keep-alive logic |
| Thread pool | Bounded producer-consumer queue, fixed worker thread pool, shutdown coordination |
| Filesystem | Safe path resolution, MIME type lookup, directory listing generation |
| Access logging | Thread-safe Common Log Format logging |
| Testing | Unit tests, integration tests, benchmark script |
| Documentation | Technical report, phase reports, README |
