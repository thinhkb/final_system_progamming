# Access Logging — Technical Report

## Overview

Every HTTP server needs to record what it serves. The logging layer (`src/log.c`) writes every completed request to a Common Log Format (CLF) file, giving operators visibility into traffic patterns, errors, and client behavior. This report covers the format specification, the thread-safety architecture, and the implementation details that make the logging correct and crash-safe.

---

## 1. Common Log Format (CLF) Specification

The Common Log Format originated with NCSA httpd and became the de facto standard for HTTP access logs. RFC 7232 (formerly RFC 2616) describes it informally. Every line has the following structure:

```
host ident authuser [timestamp] "request" status bytes
```

The server's implementation omits two fields by design:

```
127.0.0.1 - - [10/Oct/2026:13:55:36 +0000] "GET /index.html HTTP/1.1" 200 232
^^^^^^^^  ^ ^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ~~~~~~~~~~~~~~~~~~~~~~~~~ ^^ ~~
  host    | |          timestamp                    request line       |  |
      ident (dash = not implemented)                                status  |
  authuser (dash = not implemented)                                  bytes sent
```

### Field-by-Field Breakdown

| Field        | Our Log Value | Meaning                                              |
|--------------|---------------|------------------------------------------------------|
| `host`       | `127.0.0.1`   | Client IP address                                    |
| `ident`      | `-`           | Ident protocol (RFC 931) — not implemented           |
| `authuser`   | `-`           | Authenticated username — not implemented             |
| `timestamp`  | `[...]`       | Local time in CLF format: `dd/Mon/yyyy:HH:mm:ss %z` |
| `request`    | `"..."`       | Request line: `METHOD URI HTTP/VERSION`             |
| `status`     | `200`         | HTTP response status code                            |
| `bytes`      | `232`         | Bytes sent in response body (0 for errors)           |

The dashes for `ident` and `authuser` are intentional placeholders. Implementing ident lookup would require an ident daemon and add latency to every request. Authentication is not a feature of this server, so `authuser` is always `-`.

---

## 2. Timestamp Formatting

CLF requires a specific date-time format that differs from ISO 8601:

```
dd/Mon/yyyy:HH:mm:ss +zzzz
10/Oct/2026:13:55:36 +0000
```

```c
// src/log.c — Timestamp formatting (lines ~18-26)
char timestamp[64];
time_t now = time(NULL);
struct tm tm_buf;
localtime_r(&now, &tm_buf);
strftime(timestamp, sizeof(timestamp),
         "%d/%b/%Y:%H:%M:%S %z", &tm_buf);
```

The `strftime` pattern breaks down as:

| Pattern | Value   | Meaning                    |
|---------|---------|----------------------------|
| `%d`    | `10`    | Day of month, zero-padded   |
| `%b`    | `Oct`   | Abbreviated month name      |
| `%Y`    | `2026`  | 4-digit year                |
| `%H`    | `13`    | Hour (24-hour, zero-padded) |
| `%M`    | `55`    | Minute                      |
| `%S`    | `36`    | Second                      |
| `%z`    | `+0000` | UTC offset                  |

**`localtime_r`** is the thread-safe variant of `localtime`. It writes the `struct tm` into a caller-supplied buffer rather than using a shared static buffer (which `localtime` uses), making it safe to call from multiple threads concurrently.

---

## 3. Thread-Safety Challenge

The server uses a thread pool. Multiple worker threads handle requests simultaneously, and any of them may call `access_log_write()` at the same time. If two threads call `fprintf` simultaneously without synchronization:

```
Thread A: fprintf(file, "127.0.0.1 - ...") → interleaved with Thread B
Thread B: fprintf(file, "192.168.1.1 - ...")
```

The result is garbled, interleaved log lines — a data race on the FILE stream. The solution is a mutex.

---

## 4. Implementation — Mutex-Protected Logging

```c
// src/log.c — Access log structure (lines ~6-9)
struct access_log {
    FILE *file;
    pthread_mutex_t mutex;
};

// src/log.c — Opening the log (lines ~12-16)
int access_log_open(struct access_log *log, const char *path) {
    log->file = fopen(path, "a");    // Append mode — no truncation
    if (!log->file) return -1;
    pthread_mutex_init(&log->mutex, NULL);
    return 0;
}

// src/log.c — Thread-safe write (lines ~28-43)
void access_log_write(struct access_log *log,
                      const char *client_ip,
                      const char *request_line,
                      int status_code,
                      size_t bytes_sent)
{
    char timestamp[64];
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(timestamp, sizeof(timestamp),
             "%d/%b/%Y:%H:%M:%S %z", &tm_buf);

    pthread_mutex_lock(&log->mutex);
    fprintf(log->file,
            "%s - - [%s] \"%s\" %d %zu\n",
            client_ip, timestamp, request_line, status_code, bytes_sent);
    fflush(log->file);               // <-- See below
    pthread_mutex_unlock(&log->mutex);
}
```

### Why Append Mode (`"a"`)?

The file is opened with `"a"` (append). On Unix, append mode ensures that every write goes to the end of the file regardless of seeking — even if multiple processes or `O_APPEND`-aware file descriptors interleave, the OS kernel serializes the writes. The mutex provides the same guarantee within a single process.

### Why `fflush` Inside the Mutex?

This is critical for crash safety. Standard C output is **buffered**:

| Mode     | Behavior                                                    |
|----------|-------------------------------------------------------------|
| Unbuffered (`_IONBF`) | Every `fprintf` call → immediate write |
| Line-buffered (`_IOLBF`) | Written at each newline                 |
| Fully buffered (`_IOFBF`) | Written when buffer is full             |

The default for `fopen("a", ...)` is **fully buffered**. This means `fprintf` copies the log line into a kernel buffer and returns immediately — the data may not reach the disk for seconds or minutes. If the process crashes before the buffer is flushed, the last few log entries are lost.

By calling `fflush(log->file)` inside the mutex, every successful `access_log_write()` call guarantees that the line is flushed to the kernel's write buffer before the mutex is released. This minimizes log loss on crash.

**Trade-off**: `fflush` is a syscall (or at least a write-to-kernel call), which has a performance cost. For high-throughput servers, this can become a bottleneck. An alternative is to accept line-buffered mode (`setvbuf(log->file, NULL, _IOLBF, 0)`), which flushes at each `\n` automatically — but this is slightly less safe because a crash between writes can still lose data.

---

## 5. What Gets Logged

### Client IP

Passed from the connection handler. In production behind a reverse proxy, this would come from the `X-Forwarded-For` header — the server currently logs the direct TCP connection IP.

### Request Line

The full HTTP request line: method, URI, and version:

```
GET /index.html HTTP/1.1
HEAD /favicon.ico HTTP/1.1
POST /upload HTTP/1.0
```

This is the exact string the client sent, already parsed by the request handler. Note: for failed requests (malformed, 400 Bad Request), the request line may be empty or `-`.

### Status Code

The HTTP status code of the response:

| Code | Meaning              |
|------|----------------------|
| `200` | OK                   |
| `204` | No Content           |
| `304` | Not Modified         |
| `400` | Bad Request          |
| `403` | Forbidden            |
| `404` | Not Found            |
| `500` | Internal Server Error|

### Bytes Sent

The number of bytes in the response body. Key rules:
- `0` is logged for errors (400, 403, 404, 500) since no body is sent
- For 304 responses, the body is not sent, so bytes may be `0` or the file size — check the implementation
- This is the **body** size, not the total wire bytes (headers + body)

---

## 6. Integration with Request Handling

The logging call happens **after** the response is sent:

```
Request arrives → Parse → Validate path → Send file/listing/error
                                                   ↓
                                         access_log_write() ← HERE
                                                   ↓
                                         Close connection
```

This ordering is important: we log the actual outcome, not the request. If the file couldn't be found, we log 404. If it was found but the client disconnected during transfer, we still log the attempt.

---

## 7. Edge Cases and Gotchas

### Bytes Sent = 0 on 200 OK

For empty files, a 200 response with 0 bytes is correct and should log `0`.

### Large Files

For files larger than `SIZE_MAX`, `size_t` handles it. For very large files, `bytes_sent` in the log may exceed what fits in a 32-bit integer — `fprintf("%zu", ...)` handles this correctly with `size_t`.

### Log File Permissions

The log file must be writable by the worker process user. If started as root, the server should `setuid()` to a less privileged user after binding the port — otherwise log files may be owned by root and inaccessible to operators.

### Clock Skew and Timezones

`localtime_r` uses the server's local timezone. In distributed systems, logs from different servers in different timezones are hard to correlate. The `%z` offset field helps, but for production, consider UTC (`gmtime_r`) or structured JSON logging with ISO 8601 timestamps.

---

## Key Implementation Details

- **`access_log_t`** — simple struct: `FILE*` + `pthread_mutex_t`. No dynamic allocation.
- **`pthread_mutex_init`** — static initializer `PTHREAD_MUTEX_INITIALIZER` could also be used, but the init function allows returning an error code.
- **`fprintf` format `"%zu"`** — correct format specifier for `size_t`. Using `%lu` would truncate on 64-bit systems.
- **Mutex granularity** — the mutex covers only `fprintf` + `fflush`, not the `strftime` or timestamp formatting. This keeps the critical section as short as possible.
- **No dynamic allocation in `access_log_write`** — all buffers are stack-allocated. No malloc failures can occur during logging.
