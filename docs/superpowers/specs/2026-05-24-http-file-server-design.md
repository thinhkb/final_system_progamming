# HTTP File Server Final Project Design

**Goal:** Build a C11/POSIX multi-threaded HTTP file server that serves static files from a document root, supports basic browser-compatible file browsing, and ships with tests, benchmarks, README, and report material suitable for final submission.

**Architecture:** The server uses a single acceptor loop plus a fixed worker thread pool fed by a bounded producer-consumer queue. Request parsing, filesystem resolution, response formatting, logging, and concurrency control live in separate modules so each part can be tested independently. The implementation stays intentionally narrow: static content only, no CGI, no uploads, no TLS, no dynamic app layer.

**Tech Stack:** C11, POSIX sockets, pthreads, GCC on Linux x86-64, GNU Make, shell scripts, curl, and standard Unix utilities.

---

## Scope

This project targets assignment A3: HTTP file server. The codebase will deliver a working server plus the required project artifacts:
- source code
- Makefile
- sample `www/` content
- automated tests
- benchmark script
- README.md
- technical report draft

## Functional Requirements

- Accept TCP connections on a configurable host/port.
- Serve files from a configurable document root.
- Support `GET` and `HEAD`.
- Support HTTP/1.0 and HTTP/1.1 requests.
- Keep connections alive when the client and request version allow it.
- Reject unsupported methods with `501 Not Implemented`.
- Reject malformed requests with `400 Bad Request`.
- Reject path traversal outside the document root with `403 Forbidden`.
- Return `404 Not Found` for missing files.
- Serve directory listings for folders.
- Infer `Content-Type` from file extension.
- Emit access logs in Common Log Format.
- Handle concurrent clients through a bounded queue and worker pool.
- Survive a 100+ client stress test without crashing.

## Non-Goals

- CGI or FastCGI
- Uploads, deletes, or edits via HTTP
- TLS/HTTPS
- Web frameworks or external runtime dependencies
- Persistent application state beyond the document root and logs

## Module Layout

- `src/main.c`: argument parsing, signal handling, server startup/shutdown.
- `src/server.c`: socket creation, accept loop, connection dispatch.
- `src/thread_pool.c`: bounded queue, worker lifecycle, shutdown coordination.
- `src/http.c`: request parsing, method/version handling, keep-alive logic.
- `src/files.c`: document-root resolution, MIME lookup, directory listing, file reads.
- `src/log.c`: Common Log Format formatting and writeout.
- `include/*.h`: public interfaces between modules.
- `www/`: static sample content and directory-listing fixtures.
- `tests/`: black-box tests driven by shell and curl.
- `bench/`: concurrency benchmark script.
- `report/`: architecture diagrams and report draft.

## Data Flow

1. The main thread listens on the configured socket.
2. Accepted sockets enter a bounded queue.
3. Worker threads pop sockets, read one or more requests, and generate responses.
4. The HTTP layer validates syntax, version, and connection semantics.
5. The filesystem layer resolves the requested path and returns either metadata, file content, or a generated directory listing.
6. The log layer records each completed request in Common Log Format.

## Error Handling

- Parse failures return `400` and close the connection unless the request explicitly allows reuse and the parser can safely continue.
- Unsupported methods return `501`.
- Missing paths return `404`.
- Access outside the document root returns `403`.
- Internal failures return `500` with a short plaintext body.
- Queue shutdown and signal handling must stop new accepts cleanly and drain active workers before exit.

## Testing Strategy

Tests are black-box and scriptable so they can run in a clean Linux environment.

Minimum cases:
- existing file returns `200` and correct body
- `HEAD` returns headers without a body
- missing file returns `404`
- path traversal attempt returns `403`
- directory request returns an HTML listing
- unsupported method returns `501`
- HTTP/1.1 keep-alive works across two requests
- concurrent requests complete without crash

## Benchmark Strategy

A shell benchmark script will launch many concurrent clients using curl or a small loop to confirm the server handles load and remains responsive. The benchmark output will capture elapsed time and basic throughput-style observations for the report.

## Report Strategy

The report draft will follow the assignment structure:
- introduction
- background and theory
- system design
- implementation
- testing and validation
- performance analysis
- conclusion and future work
- references

It will include at least two diagrams:
- system architecture diagram
- request lifecycle / flow diagram

## Acceptance Criteria

The project is done when:
- `make`, `make clean`, and `make test` work from a fresh checkout.
- The server serves the sample content and passes the scripted tests.
- The benchmark script runs successfully.
- The README documents build, run, test, and usage steps.
- The report draft exists and maps to the required structure.
