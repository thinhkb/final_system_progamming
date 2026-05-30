## System Programming Knowledge Map

### Level 1 — Foundational (you must know cold)

| Topic                                     | What it is                                                                                                                 | Where in this project                                                             |
| ----------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| **File descriptors**                | Integer handles the kernel uses for I/O (stdin=0, stdout=1, stderr=2, then 3+ for sockets/files)                           | Every `socket()`, `accept()`, `recv()`, `send()`, `fopen()` returns one |
| **System calls vs library calls**   | System calls cross into kernel (`read`, `write`, `socket`); library calls stay in userspace (`fread`, `fprintf`) | `send()`/`recv()` are syscalls; `fread()`/`fprintf()` are libc            |
| **The** `errno` **pattern** | On failure, syscalls return -1 and set global `errno` to a code like `ENOENT`, `EFAULT`                              | `realpath()` returns NULL and sets `errno` to detect missing files            |
| **POSIX process model**             | Each process has its own address space; threads share it                                                                   | `fork()` is not used here (single process, multiple threads)                    |

### Level 2 — Networking (TCP/IP Sockets)

| Topic                                  | What it is                                                                                              | Where in this project                                                      |
| -------------------------------------- | ------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| **Socket creation**              | `socket(domain, type, protocol)` — `AF_INET` + `SOCK_STREAM` = TCP                               | `server.c:create_listening_socket()`                                     |
| **Address resolution**           | `getaddrinfo()` — converts host/port to a `struct sockaddr`                                        | Replaces the old `gethostbyname()` + `bind()` pattern                  |
| `bind()`                             | Attaches a socket to a specific IP + port                                                               | `create_listening_socket()`                                              |
| `listen()`                           | Marks a socket as passive (for accepting connections);`SOMAXCONN` = OS max backlog                    | `server.c:502`                                                           |
| `accept()`                           | Pulls the next incoming connection from the kernel queue, returns a**new socket** for that client | `server.c:557` — called in the main accept loop                         |
| `send()` **/** `recv()`      | Buffered byte-stream send/receive over a connected socket                                               | `server.c:send_all()`, `read_next_request()`                           |
| `shutdown()`                         | Half-closes a socket (shut down read or write side)                                                     | Not used, but relevant for HTTP keep-alive                                 |
| `SO_REUSEADDR`                       | Allows binding to a port immediately after the server restarts                                          | `server.c:501`                                                           |
| **Blocking vs non-blocking I/O** | By default sockets block —`recv()` waits until data arrives                                          | The server uses blocking I/O; workers block on `recv()` or queue dequeue |

**You should be able to draw the TCP 3-way handshake and explain what happens at** `socket() → bind() → listen() → accept() → recv() → send() → close()`.

### Level 3 — Threads (pthreads)

| Topic                                                                                                                 | What it is                                                  | Where in this project                                                                                                     |
| --------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `pthread_create()`                                                                                                  | Spawns a new thread — starts executing a function          | `thread_pool.c:175` — creates N worker threads                                                                         |
| `pthread_join()`                                                                                                    | Waits for a thread to finish                                | `thread_pool.c:196` — main thread waits for all workers                                                                |
| `pthread_mutex_t`                                                                                                   | A lock — only one thread can hold it at a time             | `<span class="md-inline-path-filename">thread_pool.c</span>` — protects the shared queue `count`, `head`, `tail` |
| `pthread_cond_t`                                                                                                    | A signal — threads can wait on it until another signals    | `not_empty` (workers wait when queue is empty), `not_full` (producer waits when queue is full)                        |
| `<span class="md-inline-path-prefix">pthread_mutex_lock/</span><span class="md-inline-path-filename">unlock</span>` | Acquire or release a mutex                                  | Every `enqueue`/`dequeue` call wraps these                                                                            |
| `pthread_cond_signal()`                                                                                             | Wake up**one** waiting thread                         | `thread_pool.c:106` (signal not_empty after enqueue), `:131` (signal not_full after dequeue)                          |
| `pthread_cond_broadcast()`                                                                                          | Wake up**all** waiting threads                        | `thread_pool.c:144` (shutdown broadcasts to break all waiters)                                                          |
| **Thread vs process**                                                                                           | Threads share heap/FDs/code; processes have separate memory | The pool uses one process, multiple threads — all share the socket queue                                                 |

**The critical concept here is: a mutex prevents two threads from touching the shared queue simultaneously. A condition variable lets a thread sleep efficiently instead of spinning in a** `while(queue.empty) {}` **loop.**

### Level 4 — Concurrency Patterns

| Topic                       | What it is                                                                                            | Where in this project                                                                                               |
| --------------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| **Producer-Consumer** | One thread (producer/acceptor) creates work; N threads (consumers/workers) process it                 | Main thread enqueues sockets; workers dequeue and handle them                                                       |
| **Bounded Queue**     | A fixed-size buffer between producer and consumers                                                    | `<span class="md-inline-path-filename">thread_pool.c</span>` — circular buffer with `count` and `capacity`   |
| **Backpressure**      | When the queue is full, the producer must wait or reject work                                         | `server.c:571` — `QUEUE_FULL` returns 503 and closes the socket                                                |
| **Thread Pool**       | Fixed number of pre-created threads reused across tasks (vs creating a thread per connection)         | `<span class="md-inline-path-filename">thread_pool.c</span>` — `thread_count` is set at startup, never changes |
| **Spurious wakeups**  | Condition variables can wake up for no reason, so always use a `while (condition)` loop, not `if` | `thread_pool.c:119` — `while (queue->count == 0 && !queue->shutdown)`                                          |

### Level 5 — HTTP Protocol

| Topic                            | What it is                                                                                                                                          | Where in this project                                                                                            |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| **HTTP request structure** | `METHOD PATH VERSION\r\n` + headers + `\r\n\r\n` + optional body                                                                                | `http.c:http_parse_request()`                                                                                  |
| **CRLF handling**          | HTTP uses `\r\n` (not just `\n`) as line endings                                                                                                | `http.c:find_crlf()`                                                                                           |
| **Connection: keep-alive** | In HTTP/1.0, connection closes by default unless `Connection: keep-alive`. In HTTP/1.1, it stays open unless `Connection: close`                | `http.c:266-270` — the `keep_alive_requested` logic                                                         |
| **Range requests**         | Client sends `Range: bytes=start-end`, server responds with `206 Partial Content`                                                               | `http.c:parse_range_header()`, `server.c:send_file_response()`                                               |
| **Status codes**           | 200=OK, 206=Partial Content, 400=Bad Request, 403=Forbidden, 404=Not Found, 416=Range Not Satisfiable, 501=Not Implemented, 503=Service Unavailable | `http.c:http_status_text()`                                                                                    |
| **Content-Type**           | The `Content-Type` header tells the client how to interpret the response body                                                                     | `file_mime_type()` in `<span class="md-inline-path-filename">files.c</span>` maps extensions to MIME strings |
| **Content-Length**         | The `Content-Length` header tells the client how many bytes to expect                                                                             | Every response in `<span class="md-inline-path-filename">server.c</span>` includes it                          |
| **Accept-Ranges: bytes**   | Server advertises that it supports range requests                                                                                                   | Sent in 200 responses in `<span class="md-inline-path-filename">server.c</span>`                               |

### Level 6 — File System

| Topic                                                                                 | What it is                                                                                                                              | Where in this project                                                                                                                                          |
| ------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `stat()`                                                                            | Gets metadata about a file (size, type, permissions, modification time)                                                                 | `files.c:379` — used to check if path is file or directory                                                                                                  |
| `realpath()`                                                                        | Resolves a relative or symlink-filled path to its absolute canonical form                                                               | `files.c:343,347` — used for security (prevents `<span class="md-inline-path-prefix">/../etc/</span><span class="md-inline-path-filename">passwd</span>`) |
| `opendir()` **/** `readdir()` **/** `closedir()`                    | Reads the entries of a directory                                                                                                        | `files.c:file_build_directory_listing()`                                                                                                                     |
| **Directory vs regular file**                                                   | `S_ISDIR(st.st_mode)` vs `S_ISREG(st.st_mode)`                                                                                      | `files.c:386-400`                                                                                                                                            |
| `fopen()` **/** `fread()` **/** `fseeko()` **/** `fclose()` | File I/O with buffering                                                                                                                 | `server.c:261,278,326,338` — streaming file content to socket                                                                                               |
| **Path traversal attack**                                                       | `<span class="md-inline-path-prefix">/../../etc/</span><span class="md-inline-path-filename">passwd</span>` escapes the document root | `files.c:contains_dot_dot_segment()` + `path_has_prefix()` block it                                                                                        |
| **URL decoding**                                                                | `%20` → space, `%2F` → `<span class="md-inline-path-prefix">/</span>`                                                           | `files.c:decode_path()` handles `%XX` hex decoding                                                                                                         |
| **HTML escaping**                                                               | `<`, `>`, `&`, `"` in directory listing names must become `&lt;`, `&gt;`, `&amp;`, `&quot;`                             | `files.c:builder_append_html_escaped()`                                                                                                                      |

### Level 7 — Signals and Process Lifecycle

| Topic                         | What it is                                                                                                                    | Where in this project                                                      |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| **Signal handling**     | `signal()` or `sigaction()` catches events like Ctrl+C (`SIGINT`)                                                       | `main.c:87-88` — both SIGINT and SIGTERM call `server_stop()`         |
| `volatile sig_atomic_t`     | The `should_stop` flag is read by the main loop while the signal handler writes it — this type ensures atomic reads/writes | `server.h:14` — `volatile sig_atomic_t should_stop`                   |
| **Graceful shutdown**   | Don't just `exit()` — stop accepting, drain the queue, join threads, close resources                                       | `server_stop()` → `socket_queue_shutdown()` → `thread_pool_stop()` |
| `fork()` **not used** | This project uses threads inside one process (not `fork()` which creates a new process per connection)                      | Single process, thread pool                                                |

### Level 8 — Memory and Buffers

| Topic                     | What it is                                                                                     | Where in this project                                                                                                                                      |
| ------------------------- | ---------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Heap allocation** | `malloc()`, `realloc()`, `free()`                                                        | String builder in `<span class="md-inline-path-filename">files.c</span>`, queue buffer in `<span class="md-inline-path-filename">thread_pool.c</span>` |
| **Stack vs heap**   | Local arrays on the stack (`char buffer[8192]`), dynamic data on the heap                    | `<span class="md-inline-path-filename">server.c</span>` uses stack buffers for reads; queue uses `calloc()` on the heap                                |
| **Buffer overflow** | Writing past the end of a buffer —`snprintf()` is safe, `sprintf()` is not                | All string building uses `snprintf()` or bounded helpers                                                                                                 |
| `memmove()`             | Copies memory that may overlap (needed when shifting buffered bytes after consuming a request) | `server.c:113` — `memmove(buffer, buffer + header_length, ...)`                                                                                       |
| **File streaming**  | Never load a large file entirely into memory — read and send in chunks                        | `server.c:324-336` — `while (remaining > 0) { fread + send }`                                                                                         |

---

## What to Study (Priority Order)

For the teacher presentation, you should be able to explain **these 6 things in depth**:

**1. The Thread Pool + Bounded Queue (most important)** Draw the circular buffer on the board. Show how `head` and `tail` pointers wrap around. Explain why you need both mutexes and condition variables — not just one.

**2. How TCP Socket Lifecycle Works** `socket() → bind() → listen() → accept() → recv() → send() → close()`. Show that `accept()` creates a **new fd** each time. The original listen socket just accepts.

**3. HTTP Keep-Alive Logic** HTTP/1.0 default = close, requires `Connection: keep-alive`. HTTP/1.1 default = keep-alive, requires `Connection: close`. Show the `<span class="md-inline-path-prefix">if/</span><span class="md-inline-path-filename">else</span>` in `<span class="md-inline-path-filename">http.c</span>` that sets `keep_alive_requested`.

**4. Path Security** `realpath()` resolves all symlinks and `..` segments. Then `path_has_prefix()` checks the resolved path still starts with the doc root. Draw an example: `<span class="md-inline-path-prefix">/www/listing/../etc/</span><span class="md-inline-path-filename">passwd</span>` → `realpath` gives `<span class="md-inline-path-prefix">/www/listing/../etc/</span><span class="md-inline-path-filename">passwd</span>` which is outside `<span class="md-inline-path-prefix">/www/</span>`.

**5. Producer-Consumer Synchronization** Main thread (producer) calls `enqueue()`. Workers (consumers) call `dequeue()`. If queue is empty, `dequeue()` does `pthread_cond_wait(not_empty, mutex)` — thread sleeps without burning CPU. When producer enqueues, it signals `not_empty`.

**6. Signal-Safe Shutdown** When `SIGINT` arrives, `server_stop()` sets `should_stop = 1`, closes the listen socket (unblocks `accept()`), and broadcasts to all condition variables (unblocks all `dequeue()` waits). Then `thread_pool_stop()` joins all threads.

---

## Quick Study References

| Resource                   | What to read                            |
| -------------------------- | --------------------------------------- |
| `man 7 socket`           | TCP socket options,`SO_REUSEADDR`     |
| `man 7 pthread`          | pthreads overview                       |
| `man pthread_mutex_init` | mutex and condition variable semantics  |
| `man realpath`           | path canonicalization                   |
| RFC 9112                   | HTTP/1.1 request/response format        |
| `man signal`             | signal safety,`volatile sig_atomic_t` |
