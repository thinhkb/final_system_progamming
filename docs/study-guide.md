# Study Guide: HTTP File Server — System Programming

This document covers every system programming topic tested by this project, organized for presentation prep.

---

## 1. TCP/IP Sockets

### The Socket Lifecycle

Every network connection in this project follows this exact sequence:

```
socket()   → create a file descriptor (kernel object)
    ↓
bind()     → attach the socket to a specific IP:port
    ↓
listen()   → mark as passive; allow incoming connections
    ↓
accept()   → pull next connection from kernel queue → return NEW fd per client
    ↓
recv()     → receive bytes from client
    ↓
send()     → send bytes back to client
    ↓
close()    → release the file descriptor
```

Key point: `accept()` returns a **new socket fd** for each client. The original listen socket stays open and keeps accepting new clients.

### `getaddrinfo()` vs older APIs

This project uses `getaddrinfo()` instead of `gethostbyname()` + `struct sockaddr_in`. `getaddrinfo()` is modern, thread-safe, and handles both IPv4 and IPv6.

### `SO_REUSEADDR`

```c
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
```
Without this, after the server stops and restarts, the port stays in `TIME_WAIT` state for ~60 seconds and you can't rebind to it. `SO_REUSEADDR` allows immediate rebind.

---

## 2. Threads (pthreads)

### Thread Creation and Joining

```c
pthread_t thread;
pthread_create(&thread, NULL, worker_main, args);  // spawn
pthread_join(thread, NULL);                          // wait for completion
```

`pthread_create()` starts a new thread running `worker_main(args)`. `pthread_join()` blocks until that thread finishes. In this project, the main thread creates N workers at startup and joins them all at shutdown.

### Mutex (Mutual Exclusion)

A mutex is a lock — only one thread can hold it at a time.

```c
pthread_mutex_lock(&queue->mutex);
// ... critical section: access shared data ...
pthread_mutex_unlock(&queue->mutex);
```

If two threads try to lock the same mutex, one blocks until the other unlocks. In this project, the queue uses one mutex to protect `head`, `tail`, and `count`.

### Condition Variables (Signaling)

A condition variable lets a thread **sleep** until another thread signals it.

```c
// Consumer waits until queue has data
pthread_cond_wait(&queue->not_empty, &queue->mutex);

// Producer signals after adding data
pthread_cond_signal(&queue->not_empty);
```

**Critical rule:** You must hold the mutex when calling `wait()` or `signal()`. The mutex is atomically released while sleeping and re-acquired on wake.

### Why Both Mutexes AND Condition Variables?

A mutex alone requires **spinning**: `while(queue.empty) {}` — this burns CPU constantly.

A condition variable allows **efficient sleeping**: the thread calls `wait()` and the OS scheduler removes it from the run queue until another thread signals. Zero CPU usage while waiting.

### Spurious Wakeups

`pthread_cond_wait()` can return even without a signal. Therefore always use a `while` loop:

```c
// WRONG — spurious wakeup causes wrong behavior
if (queue->count == 0) { pthread_cond_wait(...); }

// CORRECT — re-check condition after wake
while (queue->count == 0 && !queue->shutdown) {
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
}
```

---

## 3. Concurrency Pattern: Producer-Consumer

### The Problem

One thread (producer = main accept loop) generates work. Multiple threads (consumers = workers) process work. The queue between them is shared state → needs synchronization.

### The Solution in This Project

```
Main Thread (Producer)          Worker Thread 1 (Consumer)
─────────────────              ──────────────────────────
accept() → client_fd
socket_queue_enqueue()         socket_queue_dequeue() ← unblocks here
   │ (mutex + signal)                    │
   │                                     ▼
   │                              handle_client() → recv/send
   │                                     │
   │                                     ▼
   │                              process request
   │                                     │
   │                                     ▼
   │                              access_log_write()
   │                                     │
   ▼                                     ▼
next accept()                   back to queue waiting
```

### Bounded Queue (Circular Buffer)

The queue has fixed capacity. When full, the producer gets `QUEUE_FULL` and must reject the client (503 response).

```c
// Circular buffer math
queue->items[queue->tail] = client_fd;
queue->tail = (queue->tail + 1) % capacity;  // wrap around
queue->count++;
```

Both `head` and `tail` wrap with modulo, creating a ring. The mutex prevents simultaneous access. The condition variables (`not_empty`, `not_full`) enable efficient sleep/wake.

### Backpressure

When the queue is full:
```c
if (enqueue_result == QUEUE_FULL) {
    send_simple_response(client_fd, NULL, 503, ...);  // Service Unavailable
    close(client_fd);
}
```
This prevents the server from being overwhelmed — excess clients get an error instead of hanging.

---

## 4. HTTP Protocol

### Request Format (RFC 9112)

```
GET /index.html HTTP/1.1\r\n
Host: localhost\r\n
Connection: keep-alive\r\n
Range: bytes=0-1023\r\n
\r\n
```

- Line endings are `\r\n` (not `\n`)
- Blank line (`\r\n\r\n`) separates headers from body
- `Connection:` header controls keep-alive behavior

### Keep-Alive Logic

```
HTTP/1.0:  connection closes by DEFAULT  → need "Connection: keep-alive" to keep open
HTTP/1.1:  connection stays open by DEFAULT → need "Connection: close" to close
```

```c
if (request->version == HTTP_VERSION_11) {
    // Default is keep-alive; close only if explicitly requested
    request->keep_alive_requested = !ascii_case_equal(request->connection, "close");
} else {
    // Default is close; keep-alive only if explicitly requested
    request->keep_alive_requested = ascii_case_equal(request->connection, "keep-alive");
}
```

### Range Requests

Client: `Range: bytes=7-10` → Server: `206 Partial Content` with `Content-Range: bytes 7-10/36`

```c
// Suffix range: "Range: bytes=-5" means last 5 bytes
if (request->range_is_suffix) {
    range->start = file_size - suffix_length;
    range->end = file_size - 1;
}
```

If the range is invalid (e.g., start > file size): `416 Range Not Satisfiable`

### Status Codes Used

| Code | Meaning | When |
|---|---|---|
| 200 | OK | File served successfully |
| 206 | Partial Content | Range request served |
| 400 | Bad Request | Malformed request line |
| 403 | Forbidden | Path traversal attempt |
| 404 | Not Found | File does not exist |
| 416 | Range Not Satisfiable | Invalid byte range |
| 501 | Not Implemented | Unsupported method |
| 503 | Service Unavailable | Queue full (backpressure) |

---

## 5. File System Security

### Path Traversal Attack

Request: `GET /../../etc/passwd HTTP/1.1`

Without protection, this reads arbitrary files outside the document root. The server blocks this with two layers:

**Layer 1 — Reject `..` in decoded path:**
```c
static int contains_dot_dot_segment(const char *path) {
    if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
        return 1;  // BLOCKED
    }
}
```

**Layer 2 — Realpath prefix check:**
```c
char root_real[FILE_PATH_MAX];
char target_real[FILE_PATH_MAX];

realpath(doc_root, root_real);    // e.g., "/home/user/www"
realpath(joined, target_real);   // e.g., "/etc/passwd" ← outside!

if (!path_has_prefix(target_real, root_real)) {
    return FILE_RESULT_FORBIDDEN;  // BLOCKED
}
```

Even if the attacker encodes `..` as `%2e%2e`, the decoder expands it first, then `contains_dot_dot_segment()` catches it.

### HTML Escaping in Directory Listings

Directory listing names could contain `<script>alert(1)</script>`. The server must escape:
- `&` → `&amp;`
- `<` → `&lt;`
- `>` → `&gt;`
- `"` → `&quot;`

This is `builder_append_html_escaped()` in `files.c`.

---

## 6. Signal Handling and Graceful Shutdown

### The Problem

When the user presses Ctrl+C (`SIGINT`), the process receives a signal. The default action is immediate termination. We want a **graceful** shutdown instead:
1. Stop accepting new connections
2. Let active workers finish
3. Clean up resources

### The Solution

```c
// main.c
signal(SIGINT, handle_signal);
signal(SIGTERM, handle_signal);

static void handle_signal(int signum) {
    server_stop(active_server);  // non-blocking: just sets flags
}
```

The signal handler does **minimal work** — just sets `should_stop = 1` and closes the listen socket (which unblocks `accept()`). No mutex, no `printf`, no memory allocation in the handler.

### Graceful Shutdown Sequence

```
SIGINT received
    ↓
server_stop():
    should_stop = 1
    close(listen_fd)      ← accept() returns -1, loop exits
    socket_queue_shutdown() ← broadcast to all cond vars
    ↓
server_run() exits accept loop
    ↓
thread_pool_stop():
    socket_queue_shutdown()  ← broadcast not_empty (workers unblock)
    pthread_join(all threads) ← wait for all workers to finish
    ↓
server_destroy():
    fclose(log file)
    free(queue memory)
    ↓
process exits cleanly
```

---

## 7. Key Code Locations

| What | Where |
|---|---|
| Socket creation + listen | `server.c:create_listening_socket()` |
| Accept loop | `server.c:server_run()` |
| Worker thread entry | `thread_pool.c:worker_main()` |
| Queue enqueue (producer) | `thread_pool.c:socket_queue_enqueue()` |
| Queue dequeue (consumer) | `thread_pool.c:socket_queue_dequeue()` |
| HTTP request parsing | `http.c:http_parse_request()` |
| Keep-alive logic | `http.c:266-270` |
| Range header parsing | `http.c:parse_range_header()` |
| MIME type lookup | `files.c:file_mime_type()` |
| Path security check | `files.c:file_resolve_path()` |
| Directory listing HTML | `files.c:file_build_directory_listing()` |
| Access log (CLF) | `log.c:access_log_write()` |
| Signal handlers | `main.c:handle_signal()` |

---

## 8. Interview/Exam Questions

**Q: Why use a bounded queue instead of unlimited?**
A: An unlimited queue could grow forever if clients arrive faster than workers can handle them, causing out-of-memory. A bounded queue provides backpressure — when full, the producer must reject new clients with 503.

**Q: What is the difference between `pthread_cond_signal` and `pthread_cond_broadcast`?**
A: `signal` wakes one waiting thread (arbitrary choice by scheduler). `broadcast` wakes all. We use `signal` for normal operation (one producer → one consumer can proceed) and `broadcast` for shutdown (all workers need to unblock).

**Q: Why does the queue use a circular buffer instead of a linked list?**
A: Fixed capacity (bounded), contiguous memory (cache-friendly), O(1) enqueue/dequeue, no heap allocation per item (avoids malloc overhead on the hot path).

**Q: Why is `volatile sig_atomic_t` used for `should_stop`?**
A: `sig_atomic_t` guarantees reads/writes are atomic (no partial updates). `volatile` tells the compiler not to optimize away reads in the accept loop. Both are needed because the signal handler writes while the main loop reads.

**Q: How does the server handle HTTP/1.0 and HTTP/1.1 differently?**
A: Keep-alive defaults differ. HTTP/1.0 defaults to closing the connection unless `Connection: keep-alive` is sent. HTTP/1.1 defaults to keeping the connection open unless `Connection: close` is sent.

**Q: What happens if a client sends a Range header for bytes beyond the file size?**
A: The server returns `416 Range Not Satisfiable` with `Content-Range: bytes */<file_size>`.
