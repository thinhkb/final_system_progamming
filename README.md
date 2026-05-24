# System Programming HTTP File Server

Final project for System Programming, option A3: HTTP file server.

This project implements a multi-threaded HTTP/1.0 and HTTP/1.1 static file server in C. It serves files from a document root, supports Keep-Alive, MIME types, directory listings, Common Log Format access logs, and a fixed worker thread pool with a bounded producer-consumer queue.

## Group Members

Fill in the real submission information before handing in the project.

| Full name | Student ID | Email |
| --- | --- | --- |
| Member 1 | Student ID 1 | email1@example.com |
| Member 2 | Student ID 2 | email2@example.com |
| Member 3 | Student ID 3 | email3@example.com |

## Build

```bash
make
```

The build uses:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
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
```

Unsupported methods return `501 Not Implemented`, missing files return `404 Not Found`, and path traversal attempts return `403 Forbidden`.

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
- a 120-client concurrent stress check

## Benchmark

Run the standalone benchmark target:

```bash
make bench
```

The target starts the server on port `18080`, runs 120 concurrent requests against `/index.html`, reports successes, failures, elapsed time, and approximate requests per second, then stops the server.

The benchmark script can also be run against an already-running server:

```bash
./bench/bench.sh 127.0.0.1 8080 /index.html 120
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

Update this section with the real work split.

- Member 1: server architecture, socket lifecycle, and request handling.
- Member 2: HTTP parsing, filesystem safety, MIME handling, and directory listings.
- Member 3: testing, benchmark script, documentation, and report preparation.
