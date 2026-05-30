# Range Requests Implementation — Technical Report

**Phase:** 05 | **Author:** System Programming Team | **Date:** May 2026
**Status:** Complete | **Files Modified:** `src/http.c`, `src/server.c`

---

## Overview

This phase implements HTTP Range requests (RFC 9110 §14.1), enabling clients to request partial file content. This is essential for resumable downloads, streaming media, and efficient bandwidth usage in HTTP/1.1 applications.

---

## 1. HTTP Range Header Semantics (RFC 9110)

The HTTP `Range` header allows a client to request only a portion of a file:

```
Range: bytes=start-end
```

**RFC 9110 Requirements:**
- Byte ranges are zero-indexed and inclusive
- The `bytes` unit is the only standard unit (we support no others)
- Servers must respond with `416 Range Not Satisfiable` if no range is satisfiable
- Servers respond with `206 Partial Content` for satisfiable ranges
- Multiple ranges may be requested, but we support only a single range per request

### Two Range Forms Supported

| Form | Example | Meaning |
|------|---------|---------|
| **Explicit range** | `bytes=7-10` | Bytes 7, 8, 9, 10 (4 bytes) |
| **Suffix range** | `bytes=-5` | Last 5 bytes of the file |

Any comma in the Range header causes the request to be treated as invalid (we reject multi-range requests rather than degrading gracefully).

---

## 2. Range Header Parsing

**File:** `src/http.c:parse_range_header`

```c
typedef struct {
    int has_range;           // Range header present
    int range_is_suffix;     // Suffix form (bytes=-N)
    long long range_start;   // Start byte (valid if has_range)
    long long range_end;     // End byte (valid if has_range)
    int range_end_provided;  // End explicitly given vs inferred
} range_parsed_t;
```

The parser distinguishes between explicit and suffix ranges:

```c
if (range_is_suffix) {
    // "bytes=-5" → want last 5 bytes
    // actual start will be resolved later against file size
    parsed.range_start = suffix_length;
    parsed.range_end = LLONG_MAX; // sentinel
    parsed.range_end_provided = 0;
} else {
    // "bytes=7-10"
    parsed.range_start = start;
    parsed.range_end = end;
    parsed.range_end_provided = 1;
}
```

**Test cases** (`tests/unit_http.c`):

```c
test_parse_explicit_range:  Range: bytes=7-10
    → range_start=7, range_end=10, range_is_suffix=0

test_parse_suffix_range:    Range: bytes=-5
    → range_start=5, range_is_suffix=1, range_end_provided=0
```

---

## 3. Range Resolution Logic

**File:** `src/server.c:resolve_range`

Range resolution maps the parsed Range header (relative values) against the actual file size.

```c
typedef struct {
    long long start;     // Absolute start byte
    long long end;       // Absolute end byte
    long long length;    // Number of bytes to send (end - start + 1)
    int partial;         // 1 = partial response, 0 = full file
    int error;          // -1 = 416 error, 0 = ok
} range_resolved_t;
```

### Resolution Rules

```c
if (!has_range) {
    // No Range header → send entire file
    resolved->start = 0;
    resolved->end = file_size - 1;
    resolved->length = file_size;
    resolved->partial = 0;
}
```

**Suffix range** (`bytes=-N`): actual start is `max(0, file_size - suffix_length)`

```c
if (range_is_suffix) {
    long long suffix_len = range_start; // stored from parse
    if (suffix_len > file_size) {
        resolved->start = 0; // suffix exceeds file → return from beginning
    } else {
        resolved->start = file_size - suffix_len;
    }
    resolved->end = file_size - 1;
    resolved->length = resolved->end - resolved->start + 1;
    resolved->partial = 1;
}
```

**Explicit range** with clamping: `end` is capped at `file_size - 1`

```c
if (resolved->start >= file_size) {
    resolved->error = -1; // → 416 Range Not Satisfiable
}
```

If `end >= file_size`, it is silently clamped to `file_size - 1`. This matches RFC 9110: "If the last-byte-pos value is absent, or if the value is greater than or equal to the current length of the representation data, the byte range is interpreted as the remainder of the representation."

### Resolution Summary

| Scenario | start | end | Result |
|----------|-------|-----|--------|
| No Range | 0 | file_size-1 | Full (200) |
| bytes=-N, N > file_size | 0 | file_size-1 | Full (200) |
| bytes=-N, N ≤ file_size | file_size-N | file_size-1 | Partial (206) |
| bytes=7-10, file_size=100 | 7 | 10 | Partial (206) |
| bytes=9999-10000, file_size=100 | 9999 | — | 416 error |

---

## 4. 206 Partial Content Response

**File:** `src/server.c:send_file_response`

When `resolved.partial == 1`, the server responds with HTTP 206.

### Required Headers for 206

```
HTTP/1.1 206 Partial Content
Content-Type: text/html
Accept-Ranges: bytes
Content-Length: 4
Content-Range: bytes 7-10/100
```

- **`Content-Range: bytes start-end/total`** — mandatory for 206. The `/total` is the *original* file size, not the range length.
- **`Accept-Ranges: bytes`** — declares server support for range requests. Sent in both 200 and 206 responses.
- **`Content-Length`** — the range length, not the file size.

### Full vs Partial Status Determination

```c
if (resolved.partial) {
    status_code = 206;
} else {
    status_code = 200;
}
// Both include "Accept-Ranges: bytes"
```

### HEAD Requests with Range

HEAD requests receive identical headers to GET but **no body**:

```c
if (request_method == HTTP_METHOD_HEAD) {
    send_response_headers(fd, status_code, content_type, resolved.length,
                         keep_alive, resolved.partial, resolved.start,
                         resolved.end, file_size);
    return; // ← early return, no body
}
```

The range is still resolved (for header generation), but the file content is never read or sent.

---

## 5. 416 Range Not Satisfiable

**File:** `src/server.c:send_range_not_satisfiable`

Occurs when `resolved.error == -1` (i.e., `start >= file_size`).

### 416 Response Format

```
HTTP/1.1 416 Range Not Satisfiable
Content-Range: bytes */100
Content-Length: 0
```

The `Content-Range: bytes */size` header (RFC 9110 §15.5.16) indicates the actual file size without specifying a range — it tells the client what range *would* be valid.

**Why 416, not 400?** RFC 9110 specifies 416 when the range is syntactically valid but cannot be satisfied (e.g., requesting bytes 9999-10000 in a 100-byte file). A malformed Range header would be 400.

---

## 6. Chunked File Streaming with fseeko

**File:** `src/server.c:send_file_response`

Range requests require reading from the middle of a file. The server uses `fseeko()` for precise positioning:

```c
// Position file pointer to range start
if (fseeko(file, resolved.start, SEEK_SET) != 0) {
    // error handling
}
```

Then streams in 8KB chunks:

```c
#define READ_BUFFER_SIZE 8192

char buffer[READ_BUFFER_SIZE];
long long remaining = resolved.length;

while (remaining > 0) {
    size_t to_read = (remaining < READ_BUFFER_SIZE) ? (size_t)remaining : READ_BUFFER_SIZE;
    size_t n = fread(buffer, 1, to_read, file);
    if (n == 0) break;

    if (send_all(fd, buffer, n) < 0) {
        // connection error
        break;
    }
    remaining -= n;
}
```

**Why fseeko instead of fread + offset?** `fseeko()` uses a 64-bit offset, making it compatible with files larger than 2GB on 32-bit systems. Standard `fseek()` uses a 32-bit long, which overflows for large files.

**send_all loop:** Each `send()` call may transmit fewer bytes than requested. The `send_all()` helper loops until all bytes are sent:

```c
ssize_t send_all(int fd, const char *buf, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(fd, buf + total_sent, len - total_sent, 0);
        if (sent < 0) return -1;
        total_sent += sent;
    }
    return total_sent;
}
```

---

## 7. Accept-Ranges Header Strategy

The `Accept-Ranges: bytes` header is included in **every successful file response** (both 200 and 206), not just range responses.

**Why include it on full-file responses?**

1. **Client guidance**: Clients can inspect the header to determine if the server supports ranges before attempting a range request
2. **HTTP/1.1 compliance**: RFC 9110 §14.2.2 recommends servers advertise range support in all successful responses to range-capable requests
3. **Stateless correctness**: Adding it conditionally (only on 206) would require extra logic; always including it is simpler and more correct

```c
void send_response_headers(...) {
    // Accept-Ranges is sent for ALL file responses
    dprintf(fd, "Accept-Ranges: bytes\r\n");
}
```

---

## 8. Test Coverage

### Unit Tests (`tests/unit_http.c`)

| Test | Input | Expected Output |
|------|-------|----------------|
| `test_parse_explicit_range` | `Range: bytes=7-10` | `range_start=7, range_end=10` |
| `test_parse_suffix_range` | `Range: bytes=-5` | `range_is_suffix=1, range_start=5` |

### Integration Tests (`tests/run_tests.sh`)

```bash
# 206 Partial Content
curl -i -H "Range: bytes=0-5" http://127.0.0.1:18080/index.html
# Expects: HTTP/1.1 206, Content-Range: bytes 0-5/<size>, body starts with "static"

# 416 Range Not Satisfiable
curl -i -H "Range: bytes=9999-10000" http://127.0.0.1:18080/index.html
# Expects: HTTP/1.1 416 Range Not Satisfiable, Content-Range: bytes */<size>
```

---

## 9. Key Implementation Decisions

| Decision | Rationale |
|----------|-----------|
| Single range only | Multi-range responses are complex (multipart MIME) and rarely used in practice |
| Suffix clamping | If suffix > file_size, return from beginning rather than 416 |
| 64-bit offsets | fseeko ensures compatibility with large files on all platforms |
| send_all loop | POSIX send() is not guaranteed to send all bytes in one call |
| Accept-Ranges always | Advertise capability unconditionally for client guidance |
| Content-Range in 416 | RFC 9110 mandates `bytes */size` to inform client of actual size |

---

## 10. Files Changed

| File | Changes |
|------|---------|
| `src/http.c` | Added `parse_range_header()`, `range_parsed_t` struct |
| `src/server.c` | Added `resolve_range()`, modified `send_file_response()`, added `send_range_not_satisfiable()` |
| `tests/unit_http.c` | Added 2 range parsing unit tests |
| `tests/run_tests.sh` | Added 2 range integration test curl assertions |
| `Makefile` | No changes (tests integrated into existing targets) |

---

## Summary

Range request support required three components working in sequence:

1. **Parse** — extract start/end from `bytes=start-end` or interpret suffix in `bytes=-N`
2. **Resolve** — map parsed values against actual file size, clamp, and detect unsatisfiable ranges
3. **Respond** — send 206 with correct `Content-Range` and `Accept-Ranges` headers, or 416 with `Content-Range: bytes */size`

The implementation follows RFC 9110 precisely, handles edge cases (empty ranges, suffix > file size, start >= file size), and integrates cleanly with the existing file serving pipeline using `fseeko()` for positioning.
