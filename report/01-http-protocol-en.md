# HTTP Protocol & Keep-Alive Implementation
## Technical Phase Report

---

## 1. Overview

This document provides a detailed technical analysis of the HTTP protocol implementation in the multi-threaded file server, covering request parsing, HTTP version semantics, Keep-Alive connection management, header extraction, and error handling. All code references are drawn from `src/http.c` and `src/server.c`.

---

## 2. HTTP Protocol Versions

### 2.1 HTTP/1.0 vs HTTP/1.1

The server supports both HTTP/1.0 and HTTP/1.1, with fundamentally different default connection behaviors:

| Aspect | HTTP/1.0 | HTTP/1.1 |
|--------|----------|----------|
| **Default connection** | Closed after response | Kept alive |
| **Keep-Alive opt-in** | Requires `Connection: keep-alive` | Requires `Connection: close` to disable |
| **Version detection** | Exact string `"HTTP/1.0"` | Exact string `"HTTP/1.1"` |
| **Unknown version** | Rejected as `400 Bad Request` | Rejected as `400 Bad Request` |

The version is parsed in `http_parse_request()` at `src/http.c:200`:

```200:208:src/http.c
static http_version_t parse_version(const char *version_text) {
    if (strcmp(version_text, "HTTP/1.0") == 0) {
        return HTTP_VERSION_10;
    }
    if (strcmp(version_text, "HTTP/1.1") == 0) {
        return HTTP_VERSION_11;
    }
    return HTTP_VERSION_UNKNOWN;
}
```

Unknown versions (including HTTP/0.9 or malformed strings) cause `http_parse_request()` to return `HTTP_PARSE_BAD_REQUEST` at `src/http.c:260`:

```258:262:src/http.c
    request->method = parse_method(request->method_text);
    request->version = parse_version(request->version_text);
    if (request->version == HTTP_VERSION_UNKNOWN) {
        return HTTP_PARSE_BAD_REQUEST;
    }
```

---

## 3. Request Line Parsing

### 3.1 Request Line Structure

An HTTP request line has the format:

```
Method SP Request-URI SP HTTP-Version CRLF
```

Example: `GET /index.html HTTP/1.1`

The parser in `http_parse_request()` (`src/http.c:210`) performs strict validation:

1. **Locate CRLF** (`find_crlf` at line 43) — scans for `\r\n` sequence
2. **Find first space** (`memchr` at line 232) — separates method from URI
3. **Find second space** (`memchr` at line 237) — separates URI from version
4. **Validate non-empty components** — rejects empty method or URI (lines 242–251)

```226:256:src/http.c
    line_end = find_crlf(raw, length);
    if (line_end == NULL || (size_t)(line_end - raw) >= length) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    request_line_len = (size_t)(line_end - raw);
    first_space = memchr(raw, ' ', request_line_len);
    if (first_space == NULL) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    second_space = memchr(first_space + 1, ' ', request_line_len - (size_t)(first_space + 1 - raw));
    if (second_space == NULL || second_space == first_space + 1) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    if ((size_t)(first_space - raw) == 0 || (size_t)(line_end - second_space - 1) == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }
```

If any validation fails, the function returns `HTTP_PARSE_BAD_REQUEST`, which triggers a `400 Bad Request` response in `src/server.c:428`.

### 3.2 Method Parsing

The server supports exactly two methods:

| Method | Handler | Behavior |
|--------|---------|----------|
| `GET` | `HTTP_METHOD_GET` | Full response with body |
| `HEAD` | `HTTP_METHOD_HEAD` | Headers only, no body |

Any other method (POST, PUT, DELETE, OPTIONS, etc.) is parsed successfully but returns `501 Not Implemented`:

```190:198:src/http.c
static http_method_t parse_method(const char *method_text) {
    if (strcmp(method_text, "GET") == 0) {
        return HTTP_METHOD_GET;
    }
    if (strcmp(method_text, "HEAD") == 0) {
        return HTTP_METHOD_HEAD;
    }
    return HTTP_METHOD_UNSUPPORTED;
}
```

In `src/server.c:435–444`:

```435:444:src/server.c
        if (request.method == HTTP_METHOD_UNSUPPORTED) {
            send_result = send_simple_response(client_fd, &request, 501, "Not Implemented\n", keep_going, &response);
            if (send_result != 0) {
                break;
            }
            access_log_write(server->access_log, client_ip, request_line, response.status_code, response.body_bytes);
            if (!keep_going) {
                break;
            }
            continue;
        }
```

### 3.3 Query String Stripping

The request URI may contain a query string (`/path?query=value`). The parser extracts only the path portion:

```246:252:src/http.c
    target_start = first_space + 1;
    target_len = (size_t)(second_space - first_space - 1);
    query_start = memchr(target_start, '?', target_len);
    path_len = query_start == NULL ? target_len : (size_t)(query_start - target_start);
    if (path_len == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }
```

The query string is discarded — it is not stored in `http_request_t::path`. This simplifies downstream handling in the file resolver.

---

## 4. Header Parsing

### 4.1 Header Iteration

The `parse_headers()` function (`src/http.c:154`) iterates through all header lines until an empty line (blank line = end of headers, signaling the body):

```157:188:src/http.c
    while (offset < headers_len) {
        const char *line = headers + offset;
        const char *line_end = find_crlf(line, headers_len - offset);
        const char *colon;
        const char *value;
        const char *value_end;
        size_t line_len;

        if (line_end == NULL) {
            break;
        }

        line_len = (size_t)(line_end - line);
        if (line_len == 0) {
            break;
        }

        colon = memchr(line, ':', line_len);
        if (colon != NULL) {
            value = skip_spaces(colon + 1, line_end);
            value_end = trim_trailing_span(value, line_end);

            if (ascii_case_equal_bounded(line, (size_t)(colon - line), "Connection")) {
                copy_bounded(request->connection, sizeof(request->connection), value, (size_t)(value_end - value));
            } else if (ascii_case_equal_bounded(line, (size_t)(colon - line), "Range")) {
                parse_range_header(value, (size_t)(value_end - value), request);
            }
        }

        offset += line_len + 2;
    }
```

### 4.2 Connection Header

The `Connection` header controls Keep-Alive behavior. The semantics differ by HTTP version:

**HTTP/1.1** (default: keep-alive):
```266:267:src/http.c
    if (request->version == HTTP_VERSION_11) {
        request->keep_alive_requested = !ascii_case_equal(request->connection, "close");
```
Connection is kept alive unless the header value is exactly `"close"` (case-insensitive).

**HTTP/1.0** (default: close):
```268:270:src/http.c
    } else {
        request->keep_alive_requested = ascii_case_equal(request->connection, "keep-alive");
    }
```
Connection is closed unless the header value is exactly `"keep-alive"` (case-insensitive).

### 4.3 Range Header

The `Range` header supports two forms:

| Form | Example | Description |
|------|---------|-------------|
| Explicit range | `bytes=100-199` | Bytes from start to end (inclusive) |
| Suffix range | `bytes=-100` | Last 100 bytes of the file |

The parser in `parse_range_header()` (`src/http.c:93`) validates the format and rejects multi-range requests (comma present):

```115:118:src/http.c
    comma = memchr(range_spec, ',', (size_t)(value_end - range_spec));
    if (comma != NULL) {
        return;
    }
```

Suffix ranges set `range_is_suffix = 1` and store the suffix length in `range_start`:

```125:132:src/http.c
    if (dash == range_spec) {
        if (!parse_size_token(dash + 1, value_end, &parsed) || parsed == 0) {
            return;
        }
        request->has_range = 1;
        request->range_is_suffix = 1;
        request->range_start = parsed;
        return;
    }
```

---

## 5. Keep-Alive Implementation

### 5.1 Server-Side Loop

The server maintains a per-connection loop in `handle_client()` (`src/server.c:400`). After each request is processed, the loop condition checks `http_should_keep_alive()`:

```433:434:src/server.c
        keep_going = http_should_keep_alive(&request);
```

The function simply returns the `keep_alive_requested` flag:

```275:277:src/http.c
int http_should_keep_alive(const http_request_t *request) {
    return request != NULL && request->keep_alive_requested;
}
```

### 5.2 Buffered Request Handling

For Keep-Alive connections, the server uses a static buffer (`src/server.c:403`) and partial-read approach. After parsing one request, the buffer retains any remaining data already received from the client:

```113:114:src/server.c
            memmove(buffer, buffer + header_length, *buffered - header_length);
            *buffered -= header_length;
```

This ensures the next request (if pipelined) is immediately available without an additional `recv()` call.

### 5.3 Connection Header in Response

The server echoes back the appropriate `Connection` header in every response:

```161:165:src/server.c
                              "Connection: %s\r\n"
                              "\r\n",
                              response_version(request), status, http_status_text(status), body_len,
                              keep_alive ? "keep-alive" : "close");
```

---

## 6. Error Responses

### 6.1 400 Bad Request

Returned when request line parsing fails (malformed method/URI/version, missing CRLF, etc.):

```227:229:src/http.c
    line_end = find_crlf(raw, length);
    if (line_end == NULL || (size_t)(line_end - raw) >= length) {
        return HTTP_PARSE_BAD_REQUEST;
    }
```

Handled in `src/server.c:422–430`:

```422:430:src/server.c
        if (read_result < 0) {
            if (buffered > 0) {
                extract_request_line(buffer, buffered, request_line, sizeof(request_line));
            } else {
                snprintf(request_line, sizeof(request_line), "<invalid request>");
            }
            send_simple_response(client_fd, NULL, 400, "Bad Request\n", 0, &response);
            access_log_write(server->access_log, client_ip, request_line, response.status_code, response.body_bytes);
            break;
        }
```

Note: The `Connection: close` is sent explicitly (keep-alive = 0) for error responses.

### 6.2 501 Not Implemented

Returned for supported HTTP versions but unsupported methods (POST, PUT, DELETE, etc.):

```435:444:src/server.c
        if (request.method == HTTP_METHOD_UNSUPPORTED) {
            send_result = send_simple_response(client_fd, &request, 501, "Not Implemented\n", keep_going, &response);
```

### 6.3 HTTP Status Codes Supported

| Code | Text | Usage |
|------|------|-------|
| 200 | OK | Successful file/directory response |
| 206 | Partial Content | Range request served |
| 400 | Bad Request | Malformed request line |
| 403 | Forbidden | Path traversal attempt blocked |
| 404 | Not Found | File/directory not found |
| 416 | Range Not Satisfiable | Invalid range or range beyond file size |
| 500 | Internal Server Error | File read failure |
| 501 | Not Implemented | Unsupported HTTP method |
| 503 | Service Unavailable | Queue full (backpressure) |

---

## 7. Key Implementation Details

### 7.1 Case-Insensitive Header Matching

Header names are matched case-insensitively using `ascii_case_equal_bounded()` (`src/http.c:27`), which compares each character after converting to lowercase:

```27:41:src/http.c
static int ascii_case_equal_bounded(const char *text, size_t text_len, const char *expected) {
    size_t expected_len = strlen(expected);

    if (text_len != expected_len) {
        return 0;
    }

    for (size_t i = 0; i < text_len; i++) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)expected[i])) {
            return 0;
        }
    }

    return 1;
}
```

### 7.2 Safe String Handling

All parsed strings use bounded copy (`copy_bounded()` at `src/http.c:7`) to prevent buffer overflows:

```7:14:src/http.c
static void copy_bounded(char *dest, size_t dest_size, const char *src, size_t src_len) {
    size_t copy_len = src_len;
    if (copy_len >= dest_size) {
        copy_len = dest_size - 1;
    }
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}
```

### 7.3 Overflow Prevention in Range Parsing

The `parse_size_token()` function (`src/http.c:72`) includes overflow checks:

```82:85:src/http.c
        if (parsed > (SIZE_MAX - (size_t)(*cursor - '0')) / 10U) {
            return 0;
        }
        parsed = parsed * 10U + (size_t)(*cursor - '0');
```

---

## 8. Summary

The HTTP implementation provides:

- **Strict request line parsing** with explicit validation of all three components
- **Correct HTTP/1.0 and HTTP/1.1 Keep-Alive semantics** based on version-specific defaults and `Connection` header values
- **Query string stripping** for clean path handling
- **Single-range support** for partial file downloads
- **Clear error responses** (400 for malformed requests, 501 for unsupported methods)
- **Safe bounded string operations** throughout to prevent buffer overflows

All parsing is done without dynamic allocation, using fixed-size buffers and explicit bounds checking throughout.
