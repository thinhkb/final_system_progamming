# Level 5 Deep Dive — HTTP Protocol

Level 5 là nơi chúng ta hiểu cách dữ liệu HTTP được truyền qua TCP socket — từ bytes thô nhận được qua `recv()` cho đến response được gửi qua `send()`. HTTP là protocol tầng ứng dụng (Layer 7), và project này triển khai HTTP/1.1 server tối giản.

---

## Mục lục

1. [HTTP Request Structure — Byte Layout](#1-http-request-structure--byte-layout)
2. [CRLF Handling — `\r\n` là bắt buộc](#2-crlf-handling--rn-là-bắt-buộc)
3. [HTTP Parsing — Từ bytes thô đến struct](#3-http-parsing--từ-bytes-thô-đến-struct)
4. [Keep-Alive Logic — HTTP/1.0 vs HTTP/1.1](#4-keep-alive-logic--http10-vs-http11)
5. [Range Requests — Partial Content 206](#5-range-requests--partial-content-206)
6. [Status Codes — 200, 206, 400, 403, 404, 416, 500, 503](#6-status-codes--200-206-400-403-404-416-500-503)
7. [Content-Type và MIME Types](#7-content-type-và-mime-types)
8. [Content-Length — Biết trước kích thước](#8-content-length--biết-trước-kích-thước)
9. [HTTP Response Building — Byte Layout](#9-http-response-building--byte-layout)
10. [File Streaming — Chunked Delivery](#10-file-streaming--chunked-delivery)
11. [HEAD method — Response không body](#11-head-method--response-không-body)

---

## 1. HTTP Request Structure — Byte Layout

### 1.1 Byte layout của một HTTP Request

```
┌──────────────────────────────────────────────────────────────────────┐
│  HTTP Request từ client gửi đến server:                              │
│                                                                      │
│  "GET /index.html HTTP/1.1\r\n"           ← Request Line           │
│  "Host: localhost:8080\r\n"               ← Header                 │
│  "Connection: keep-alive\r\n"             ← Header                 │
│  "\r\n"                                   ← Empty Line (CRLF)      │
│                                                                      │
│  Total bytes received qua recv():                                   │
│  "GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\nConnection: keep-alive\r\n\r\n" │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.2 Các thành phần

```
┌─────────────────┬─────────────────────────────────┬─────────────────┐
│  Request Line   │  Headers (0 or more)             │  Empty Line     │
│                 │                                  │                 │
│  METHOD SP PATH │  Header-Name: Header-Value\r\n  │  \r\n          │
│  \r\n          │  Header-Name: Header-Value\r\n  │                 │
│                 │  ...                            │                 │
│                 │  \r\n                          │                 │
└─────────────────┴─────────────────────────────────┴─────────────────┘
     ↑                    ↑                               ↑
  CR+LF để kết thúc  CR+LF phân tách           Double CRLF kết thúc
```

### 1.3 Request trong project

```c
// src/server.c:99–133
// recv() nhận bytes vào buffer, sau đó parse:

static int read_next_request(int client_fd, char *buffer, size_t *buffered,
                            http_request_t *request,
                            char *request_line, size_t request_line_size) {

    while (1) {
        // Bước 1: Tìm header end (\r\n\r\n)
        if (find_header_end(buffer, *buffered, &header_length)) {
            // Header complete → parse request line + headers
            extract_request_line(buffer, header_length, request_line, request_line_size);
            if (http_parse_request(buffer, header_length, request) != HTTP_PARSE_OK) {
                return -1;
            }
            // Shift body (nếu có) về đầu buffer
            memmove(buffer, buffer + header_length, *buffered - header_length);
            *buffered -= header_length;
            return 1;  // → Có request sẵn sàng
        }

        // Bước 2: Chưa complete → tiếp tục recv()
        ssize_t received = recv(client_fd,
                                 buffer + *buffered,
                                 READ_BUFFER_SIZE - *buffered, 0);
        // ...
    }
}
```

---

## 2. CRLF Handling — `\r\n` là bắt buộc

### 2.1 Tại sao HTTP dùng `\r\n`?

```
RFC 9112 Section 2:
  "A request-line and a header field line each end with a CRLF."

RFC về origin HTTP (1999):
  Mục đích: tương thích với早期的 Unix text files (chỉ dùng \n)
  \r\n (CRLF) = carriage return + line feed
  → DOS/Windows: CRLF là end-of-line
  → Unix: LF là end-of-line
  → HTTP: buộc CRLF để cross-platform
```

### 2.2 `find_crlf()` — Tìm CRLF

```c
// src/http.c:43–55
static const char *find_crlf(const char *text, size_t length) {
    if (text == NULL || length < 2) {
        return NULL;
    }

    for (size_t i = 0; i + 1 < length; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') {
            return text + i;  // Trả về con trỏ đến '\r'
        }
    }
    return NULL;
}
```

**Logic:**
- Duyệt từng byte, tìm `\r` theo sau bởi `\n`
- Trả về con trỏ đến `\r` (không phải `\n`)
- `text + i` → pointer arithmetic → vị trí của `\r` trong buffer

### 2.3 Tại sao tìm `\r\n\r\n` (double CRLF)?

```
Single request HTTP:
  "GET / HTTP/1.1\r\n"
  "Host: localhost\r\n"
  "\r\n"  ← empty line = kết thúc headers
           ↑
           │ Đây là byte thứ 2 của \r\n cuối cùng
           │ \r\n\r\n = 4 bytes cuối cùng

find_header_end() tìm: buffer chứa "\r\n\r\n"
→ Trả về pointer đến byte đầu tiên (\r)
→ Kết quả: header_length = vị trí của \r đầu tiên
→ Dùng header_length để:
  1. Copy request line
  2. Parse headers
  3. Shift body (nếu có) về đầu buffer
```

### 2.4 Trong code

```c
// src/server.c:85–97
static int find_header_end(const char *buffer, size_t buffered, size_t *header_end) {
    for (size_t i = 0; i + 1 < buffered; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            if (i + 3 < buffered &&
                buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                *header_end = i + 2;  // ← \r\n\r\n
                return 1;
            }
        }
    }
    return 0;
}
```

**Logic:**
1. Tìm `\r\n` đầu tiên
2. Kiểm tra 2 bytes tiếp theo có phải `\r\n` không
3. Nếu đúng → tìm thấy header end

---

## 3. HTTP Parsing — Từ bytes thô đến struct

### 3.1 `http_request_t` — Data structure

```c
// include/http.h:28–41
typedef struct {
    char method_text[HTTP_METHOD_TEXT_MAX];   // "GET", "HEAD"
    char path[HTTP_PATH_MAX];                  // "/index.html"
    char version_text[HTTP_VERSION_TEXT_MAX];   // "HTTP/1.1"
    char connection[HTTP_CONNECTION_TEXT_MAX];  // "keep-alive" hoặc "close"
    http_method_t method;                     // Enum: GET, HEAD, UNSUPPORTED
    http_version_t version;                   // Enum: 1.0, 1.1, UNKNOWN
    int keep_alive_requested;                  // Logic dựa trên version + Connection header
    int has_range;                            // Có Range header không
    int range_is_suffix;                      // Range dạng "-500" (suffix)
    int range_end_provided;                   // Range có end không
    size_t range_start;                       // Start của range
    size_t range_end;                         // End của range
} http_request_t;
```

### 3.2 Parsing Request Line

```
Input bytes: "GET /index.html?page=1 HTTP/1.1\r\n..."

Bước 1: Tìm CRLF đầu tiên
  → line_end = pointer đến '\r'

Bước 2: Tìm space thứ nhất
  → first_space = pointer đến ' ' sau "GET"
  → method_text = "GET"

Bước 3: Tìm space thứ hai
  → second_space = pointer đến ' ' sau "/index.html?page=1"
  → path = "/index.html?page=1" (trước space)

Bước 4: Lấy version
  → version_text = "HTTP/1.1" (sau second_space)
```

```c
// src/http.c:234–257
first_space = memchr(raw, ' ', request_line_len);
second_space = memchr(first_space + 1, ' ', request_line_len - (size_t)(first_space + 1 - raw));

// Copy method
copy_bounded(request->method_text, sizeof(request->method_text), raw, (size_t)(first_space - raw));

// Copy path
target_start = first_space + 1;
target_len = (size_t)(second_space - first_space - 1);

// Loại bỏ query string (?page=1)
query_start = memchr(target_start, '?', target_len);
path_len = query_start == NULL ? target_len : (size_t)(query_start - target_start);

copy_bounded(request->path, sizeof(request->path), target_start, path_len);

// Copy version
copy_bounded(request->version_text, sizeof(request->version_text), second_space + 1, (size_t)(line_end - second_space - 1));
```

### 3.3 Parse Method

```c
// src/http.c:190–198
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

**Project chỉ hỗ trợ GET và HEAD:**
- GET: lấy resource, có body trong response
- HEAD: lấy headers, KHÔNG có body (dùng để kiểm tra file mà không tải)
- POST/PUT/DELETE: không hỗ trợ → HTTP 501

### 3.4 Parse Headers

```c
// src/http.c:154–188
static void parse_headers(const char *headers, size_t headers_len, http_request_t *request) {
    size_t offset = 0;

    while (offset < headers_len) {
        const char *line = headers + offset;
        const char *line_end = find_crlf(line, headers_len - offset);

        if (line_end == NULL) break;
        if ((size_t)(line_end - line) == 0) break;  // Empty line = end of headers

        // Tìm ':' để tách name và value
        colon = memchr(line, ':', line_len);
        if (colon != NULL) {
            // Parse Connection header
            if (ascii_case_equal_bounded(line, (size_t)(colon - line), "Connection")) {
                copy_bounded(request->connection, sizeof(request->connection),
                           value, (size_t)(value_end - value));
            }
            // Parse Range header
            else if (ascii_case_equal_bounded(line, (size_t)(colon - line), "Range")) {
                parse_range_header(value, (size_t)(value_end - value), request);
            }
        }

        offset += line_len + 2;  // +2 cho CRLF
    }
}
```

---

## 4. Keep-Alive Logic — HTTP/1.0 vs HTTP/1.1

### 4.1 Sự khác biệt giữa HTTP/1.0 và HTTP/1.1

```
HTTP/1.0 Default Behavior:
  Mỗi request → 1 TCP connection → đóng connection
  → Request 1: SYN → ... → FIN (1 RTT connection + 1 RTT data)
  → Request 2: SYN → ... → FIN (1 RTT connection + 1 RTT data)
  → Total: 4 RTT cho 2 requests
  → OVERHEAD LỚN

HTTP/1.1 Default Behavior:
  Connection mặc định KEEP-ALIVE
  → Request 1: SYN → ... → Response (1 RTT)
  → Request 2: Response (1 RTT) — cùng connection
  → Request 3: Response (1 RTT) — cùng connection
  → Total: 1 RTT cho N requests
  → OVERHEAD THẤP
```

### 4.2 Logic trong code

```c
// src/http.c:274–281
if (request->version == HTTP_VERSION_11) {
    // HTTP/1.1: Mặc định KEEP-ALIVE
    // Trừ khi client nói rõ "Connection: close"
    request->keep_alive_requested = !ascii_case_equal(request->connection, "close");
} else {
    // HTTP/1.0: Mặc định CLOSE
    // Chỉ KEEP-ALIVE nếu client nói "Connection: keep-alive"
    request->keep_alive_requested = ascii_case_equal(request->connection, "keep-alive");
}
```

### 4.3 Truth table

| HTTP Version | Connection Header | keep_alive_requested |
|-------------|------------------|---------------------|
| HTTP/1.1 | (không có) | `true` ← Mặc định KEEP-ALIVE |
| HTTP/1.1 | `keep-alive` | `true` |
| HTTP/1.1 | `close` | `false` ← Client muốn đóng |
| HTTP/1.0 | (không có) | `false` ← Mặc định CLOSE |
| HTTP/1.0 | `keep-alive` | `true` |
| HTTP/1.0 | `close` | `false` |

### 4.4 Keep-Alive trong response

```c
// src/server.c:298–306 — File response
header_len = snprintf(headers, sizeof(headers),
    "%s 200 OK\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Accept-Ranges: bytes\r\n"
    "Connection: %s\r\n"        // ← Set connection header
    "\r\n",
    response_version(request),
    info->mime_type,
    info->size,
    keep_alive ? "keep-alive" : "close");  // ← Dựa trên keep_alive flag
```

### 4.5 Keep-Alive loop trong handle_client

```c
// src/server.c:380–413
do {
    // 1. Đọc request từ buffer hoặc recv()
    int parse_result = read_next_request(client_fd, buffer, &buffered,
                                        &request, request_line, sizeof(request_line));
    if (parse_result <= 0) {
        break;  // Connection error hoặc closed
    }

    // 2. Xử lý request...
    int status = handle_path(client_fd, &request, buffer, sizeof(buffer));

    // 3. Lấy keep_alive flag cho request tiếp theo
    keep_alive = http_should_keep_alive(&request);

} while (keep_alive);  // ← Nếu keep_alive, loop lại đọc request tiếp
```

**Flow:**
```
Client                                    Server
  │                                         │
  │── GET /index.html HTTP/1.1 ────────────→│
  │←── 200 OK + body ───────────────────────│
  │── GET /style.css HTTP/1.1 ─────────────→│  (cùng connection)
  │←── 200 OK + body ───────────────────────│
  │── GET /script.js HTTP/1.1 ─────────────→│  (cùng connection)
  │←── 200 OK + body ───────────────────────│
  │── GET /favicon.ico HTTP/1.1 ───────────→│
  │←── 200 OK + body ───────────────────────│
  │                                         │
  │ (Client đóng hoặc server đóng)          │
  │── FIN ──────────────────────────────────→│
```

---

## 5. Range Requests — Partial Content 206

### 5.1 Tại sao cần Range Requests?

```
VIDEO STREAMING:
  Client muốn xem video từ 1:30
  → Gửi Range: bytes=90000-
  → Server trả 206 với bytes 90000-EOF
  → Không cần tải 90KB đầu

DOWNLOAD RESUME:
  Download bị interrupt ở 50MB/100MB
  → Gửi Range: bytes=50000000-
  → Server trả bytes 50M-EOF
  → Tiếp tục download không cần từ đầu

PARALLEL DOWNLOAD:
  Chrome: chia file thành 3 ranges
  Range 1: bytes=0-33333 (thread 1)
  Range 2: bytes=33334-66666 (thread 2)
  Range 3: bytes=66667-99999 (thread 3)
  → Tải song song, nhanh hơn
```

### 5.2 Range header formats

```
Range: bytes=0-499            ← First 500 bytes
Range: bytes=500-999          ← Second 500 bytes
Range: bytes=-500             ← Last 500 bytes (suffix)
Range: bytes=500-             ← From byte 500 to end
```

### 5.3 Parsing Range header

```c
// src/http.c:93–152
static void parse_range_header(const char *value, size_t value_len, http_request_t *request) {
    // Kiểm tra prefix "bytes="
    if (!ascii_case_equal_bounded(value, strlen("bytes="), "bytes=")) {
        return;  // Không phải bytes range
    }

    range_spec = value + strlen("bytes=");

    // Kiểm tra có comma không (multipart ranges - không hỗ trợ)
    comma = memchr(range_spec, ',', ...);
    if (comma != NULL) {
        return;  // Multipart ranges không hỗ trợ
    }

    dash = memchr(range_spec, '-', ...);
    if (dash == NULL) {
        return;  // Không có dash
    }

    // Case: "-500" (suffix range)
    if (dash == range_spec) {
        request->has_range = 1;
        request->range_is_suffix = 1;
        request->range_start = parsed;
        return;
    }

    // Case: "500-999" (explicit range)
    if (!parse_size_token(range_spec, dash, &parsed)) {
        return;
    }
    request->has_range = 1;
    request->range_start = parsed;

    if (dash + 1 == value_end) {
        return;  // "500-" → từ byte 500 đến end
    }

    if (!parse_size_token(dash + 1, value_end, &parsed)) {
        request->has_range = 0;
        return;
    }

    request->range_end_provided = 1;
    request->range_end = parsed;
}
```

### 5.4 416 Range Not Satisfiable

```c
// src/server.c:218–257
static int resolve_range(const http_request_t *request, size_t file_size, byte_range_t *range) {
    range->partial = 0;

    if (!request->has_range) {
        return 0;  // Không có range → full file
    }

    range->start = request->range_start;
    range->end = request->range_end;
    range->partial = 1;

    if (request->range_is_suffix) {
        // Suffix range: "-500" → bytes file_size-500 to file_size-1
        if (request->range_start >= file_size) {
            return -1;  // → 416 Range Not Satisfiable
        }
        range->start = file_size - request->range_start;
        range->end = file_size - 1;
        range->length = request->range_start;
        return 0;
    }

    if (request->range_end_provided) {
        if (range->start > range->end) {
            return -1;  // → 416
        }
        if (range->end >= file_size) {
            return -1;  // → 416
        }
        range->length = range->end - range->start + 1;
    } else {
        range->end = file_size - 1;
        range->length = file_size - range->start;
    }

    if (range->start >= file_size) {
        return -1;  // → 416
    }

    return 0;
}
```

### 5.5 206 Partial Content Response

```c
// src/server.c:285–296
if (range.partial) {
    header_len = snprintf(headers, sizeof(headers),
                          "%s 206 %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Content-Range: bytes %zu-%zu/%zu\r\n"   // ← Thêm Content-Range
                          "Accept-Ranges: bytes\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          response_version(request),
                          http_status_text(206),
                          info->mime_type,
                          range.length,
                          range.start, range.end, info->size,     // ← bytes start-end/file_size
                          keep_alive ? "keep-alive" : "close");
}
```

**Example:**
```
Request:
  GET /video.mp4 HTTP/1.1
  Range: bytes=1000000-

Response:
  HTTP/1.1 206 Partial Content
  Content-Type: video/mp4
  Content-Length: 29999999
  Content-Range: bytes 1000000-30999998/30999999
  Accept-Ranges: bytes
  Connection: keep-alive
```

---

## 6. Status Codes — 200, 206, 400, 403, 404, 416, 500, 503

### 6.1 Status code categories

```
1xx: Informational (không dùng trong project)
2xx: Success
  200: OK — request thành công, có body
  206: Partial Content — trả về range của file
3xx: Redirection (không dùng trong project)
4xx: Client Error
  400: Bad Request — HTTP parse lỗi
  403: Forbidden — không có quyền đọc file
  404: Not Found — file không tồn tại
  416: Range Not Satisfiable — range vượt file size
5xx: Server Error
  500: Internal Server Error — lỗi server (fopen fail)
  501: Not Implemented — method không hỗ trợ
  503: Service Unavailable — queue full (backpressure)
```

### 6.2 `http_status_text()` — Human-readable

```c
// src/http.c:290–313
const char *http_status_text(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}
```

### 6.3 Khi nào trả status code nào?

```
200 OK:
  → File tồn tại, readable, gửi thành công

206 Partial Content:
  → Range header hợp lệ, trả phần của file

400 Bad Request:
  → http_parse_request() trả HTTP_PARSE_BAD_REQUEST
  → Request line không đúng format
  → Method không hỗ trợ (không phải GET/HEAD)

403 Forbidden:
  → stat() thành công nhưng S_ISREG/S_ISDIR fail
  → File tồn tại nhưng không phải regular file/directory

404 Not Found:
  → File không tồn tại (realpath trả NULL, errno=ENOENT)
  → Path có ".." vượt ra ngoài document root

416 Range Not Satisfiable:
  → resolve_range() trả -1
  → Range start >= file_size
  → Range end >= file_size

500 Internal Server Error:
  → fopen() fail (errno khác ENOENT/ENOTDIR)
  → fseeko() fail

501 Not Implemented:
  → Method là HTTP_METHOD_UNSUPPORTED

503 Service Unavailable:
  → Queue full (backpressure)
```

---

## 7. Content-Type và MIME Types

### 7.1 MIME Type mapping

```c
// src/files.c:48–70
static const mime_entry_t MIME_TYPES[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".txt",  "text/plain"},
    {".pdf",  "application/pdf"},
    {".zip",  "application/zip"},
    {".tar",  "application/x-tar"},
    {".gz",   "application/gzip"},
    // Default: application/octet-stream
};
```

### 7.2 MIME type detection

```c
// src/files.c:72–93
const char *file_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');  // Lấy extension cuối cùng
    if (ext == NULL) {
        return "application/octet-stream";
    }

    for (size_t i = 0; i < sizeof(MIME_TYPES) / sizeof(MIME_TYPES[0]); i++) {
        if (strcmp(ext, MIME_TYPES[i].extension) == 0) {
            return MIME_TYPES[i].mime_type;
        }
    }

    return "application/octet-stream";
}
```

### 7.3 Content-Type trong response

```c
// src/server.c:298–306
header_len = snprintf(headers, sizeof(headers),
    "%s 200 OK\r\n"
    "Content-Type: %s\r\n"   // ← MIME type
    "Content-Length: %zu\r\n"
    "Accept-Ranges: bytes\r\n"
    "Connection: %s\r\n"
    "\r\n",
    response_version(request),
    info->mime_type,      // ← từ file_mime_type()
    info->size,
    keep_alive ? "keep-alive" : "close");
```

---

## 8. Content-Length — Biết trước kích thước

### 8.1 Tại sao cần Content-Length?

```
TCP là BYTE STREAM:
  → recv() có thể trả 1 byte hoặc N bytes
  → Không có message boundary

Client nhận response:
  1. Đọc headers cho đến \r\n\r\n
  2. Đọc Content-Length bytes
  3. Đó là body

Nếu không có Content-Length:
  → Client không biết body kết thúc ở đâu
  → Connection close = end of body (EOF)
  → Không thể dùng keep-alive (vì close = end signal)
```

### 8.2 Content-Length trong code

```c
// src/server.c:298–306
header_len = snprintf(headers, sizeof(headers),
    "Content-Length: %zu\r\n"  // ← File size
    ...

// src/files.c:379
stat(path, &st);
info->size = (size_t)st.st_size;  // ← Lấy file size từ stat()
```

### 8.3 HEAD method — không có body

```c
// src/server.c:315–322
if (request->method == HTTP_METHOD_HEAD) {
    // HEAD: trả headers nhưng KHÔNG gửi body
    fclose(file);
    if (result != NULL) {
        result->status_code = status;  // 200 hoặc 206
        result->body_bytes = 0;
    }
    return 0;  // → Gửi headers, không gửi body
}
```

---

## 9. HTTP Response Building — Byte Layout

### 9.1 Response format

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 1234\r\n
Accept-Ranges: bytes\r\n
Connection: keep-alive\r\n
\r\n
<body bytes 1234 bytes>
```

### 9.2 `snprintf()` cho headers

```c
// src/server.c:298–307
int header_len = snprintf(headers, sizeof(headers),
    "%s 200 OK\r\n"                  // HTTP-Version Status-Code Reason-Phrase
    "Content-Type: %s\r\n"           // Header-Name: Header-Value
    "Content-Length: %zu\r\n"
    "Accept-Ranges: bytes\r\n"       // Server hỗ trợ range requests
    "Connection: %s\r\n"
    "\r\n",                          // Empty line = end of headers
    response_version(request),
    info->mime_type,
    info->size,
    keep_alive ? "keep-alive" : "close");
```

### 9.3 `response_version()`

```c
// src/server.c:69–77
static const char *response_version(const http_request_t *request) {
    if (request == NULL) {
        return "HTTP/1.1";  // Default
    }
    switch (request->version) {
        case HTTP_VERSION_10:
            return "HTTP/1.0";
        case HTTP_VERSION_11:
            return "HTTP/1.1";
        default:
            return "HTTP/1.1";
    }
}
```

### 9.4 Sending response

```c
// src/server.c:309–313
if (send_all(fd, headers, (size_t)header_len) != 0) {
    fclose(file);
    return -1;
}

// Tiếp tục: đọc file và gửi body
```

---

## 10. File Streaming — Chunked Delivery

### 10.1 Vấn đề: File lớn

```
Nếu load toàn bộ file vào memory:
  File 1GB → 1GB RAM → OOM

Nếu gửi 1 lần send():
  send(fd, file_buffer, 1GB, 0)
  → TCP buffer đầy → send() blocks
  → Có thể gửi không đủ bytes
```

### 10.2 Chunked delivery trong code

```c
// src/server.c:324–336
while (remaining > 0) {
    size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

    // Đọc chunk từ file (fread có buffering)
    size_t got = fread(buffer, 1, want, file);

    if (got == 0) {
        fclose(file);
        return -1;  // Lỗi đọc file
    }

    // Gửi chunk qua socket
    if (send_all(fd, buffer, got) != 0) {
        fclose(file);
        return -1;  // Client đóng connection
    }

    remaining -= got;  // Giảm số bytes còn lại
}
```

### 10.3 Data flow

```
┌──────────────────────────────────────────────────────────────┐
│  File (đĩa)                                               │
│  ↓ fread(chunk=8KB)                                        │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  User buffer (stack, 8KB)                            │  │
│  └──────────────────────────────────────────────────────┘  │
│  ↓ send_all()                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  TCP TX Buffer (kernel, ~200KB)                     │  │
│  └──────────────────────────────────────────────────────┘  │
│  ↓ NIC driver → Network                                   │
│                                                              │
│  Client: recv() ← TCP RX Buffer ← Network                  │
└──────────────────────────────────────────────────────────────┘
```

### 10.4 `fread()` vs `read()`

```c
// fread() — buffered C library
size_t got = fread(buffer, 1, want, file);
// → Dùng internal buffer (thường 4KB-8KB)
// → Ít syscall hơn (read khi buffer hết)
// → Phù hợp cho sequential reads

// read() — direct syscall
ssize_t got = read(fd, buffer, want);
// → Mỗi call = syscall
// → Dùng khi cần kiểm soát chính xác
// → Thường dùng cho non-blocking I/O
```

---

## 11. HEAD method — Response không body

### 11.1 HEAD method semantics

```
HEAD /index.html HTTP/1.1

Response:
  HTTP/1.1 200 OK
  Content-Type: text/html; charset=utf-8
  Content-Length: 1234
  Accept-Ranges: bytes
  Connection: keep-alive

  (NO BODY)

→ Client biết file size mà không tải body
→ Dùng để: kiểm tra file có tồn tại không, check ETag, check size
```

### 11.2 HEAD trong code

```c
// src/server.c:315–322
if (request->method == HTTP_METHOD_HEAD) {
    fclose(file);  // Đóng file (không đọc)
    if (result != NULL) {
        result->status_code = status;  // 200 hoặc 206
        result->body_bytes = 0;        // Không có body
    }
    return 0;  // → Headers đã gửi ở trên, không gửi body
}
```

### 11.3 HEAD với Range

```
HEAD /large-file.mp4 HTTP/1.1
Range: bytes=0-0

Response:
  HTTP/1.1 206 Partial Content
  Content-Type: video/mp4
  Content-Length: 1        ← Chỉ 1 byte
  Content-Range: bytes 0-0/30999999
  (NO BODY)

→ HEAD với range: vẫn trả 206 + Content-Range
→ Body: 0 bytes
```

---

## Tổng kết Level 5 — Quick Reference

```
┌─────────────────────────────────────────────────────────────┐
│ HTTP REQUEST STRUCTURE                                    │
│                                                             │
│  "GET /path HTTP/1.1\r\n"                               │
│  "Header: value\r\n"                                      │
│  "\r\n"                                                  │
│  [body]                                                   │
│                                                             │
│  CRLF (\r\n) là end-of-line                               │
│  Double CRLF (\r\n\r\n) kết thúc headers                │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ KEEP-ALIVE LOGIC                                          │
│                                                             │
│  HTTP/1.1: Mặc định keep-alive                          │
│    Connection: close → đóng                           │
│                                                             │
│  HTTP/1.0: Mặc định close                               │
│    Connection: keep-alive → giữ kết nối                │
│                                                             │
│  do {                                                     │
│    read_request();                                        │
│    handle();                                             │
│  } while (keep_alive);                                   │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ RANGE REQUESTS                                            │
│                                                             │
│  Range: bytes=0-499        → First 500 bytes              │
│  Range: bytes=-500         → Last 500 bytes               │
│  Range: bytes=500-         → From byte 500 to end        │
│                                                             │
│  206 Partial Content + Content-Range: bytes start-end/size │
│  416 Range Not Satisfiable nếu range >= file_size        │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ HTTP RESPONSE                                             │
│                                                             │
│  HTTP/1.1 200 OK\r\n                                     │
│  Content-Type: %s\r\n                                     │
│  Content-Length: %zu\r\n                                  │
│  Accept-Ranges: bytes\r\n                                 │
│  Connection: keep-alive\r\n                               │
│  \r\n                                                    │
│  [body bytes]                                             │
│                                                             │
│  HEAD: headers same, NO body                              │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ STATUS CODES                                              │
│                                                             │
│  200: OK                    404: Not Found                │
│  206: Partial Content       416: Range Not Satisfiable    │
│  400: Bad Request           500: Internal Server Error    │
│  403: Forbidden             501: Not Implemented          │
│                            503: Service Unavailable       │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ FILE STREAMING                                            │
│                                                             │
│  while (remaining > 0) {                                  │
│    got = fread(buffer, 1, want, file);                   │
│    send_all(fd, buffer, got);                            │
│    remaining -= got;                                     │
│  }                                                       │
│                                                             │
│  Chunk size = min(remaining, sizeof(buffer)) = 8KB     │
└─────────────────────────────────────────────────────────────┘
```
