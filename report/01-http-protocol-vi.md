# HTTP Protocol & Keep-Alive — Tóm tắt phỏng vấn

---

## Tóm tắt

Server này implement HTTP/1.0 và HTTP/1.1, hỗ trợ Keep-Alive, Range request, và parsing request line nghiêm ngặt. Điểm khác biệt cốt lõi giữa hai phiên bản HTTP nằm ở cách xử lý `Connection` header.

---

## So sánh HTTP/1.0 vs HTTP/1.1 Keep-Alive

| | HTTP/1.0 | HTTP/1.1 |
|---|---|---|
| **Mặc định** | Đóng kết nối | Giữ kết nối (keep-alive) |
| **Muốn keep-alive** | Cần header `Connection: keep-alive` | Cần header `Connection: close` để đóng |
| **Xử lý trong code** | (`src/http.c:268–269`) | (`src/http.c:266–267`) |

Code xử lý:

```c
if (request->version == HTTP_VERSION_11) {
    // 1.1: keep-alive TRỪ khi có "close"
    request->keep_alive_requested = !ascii_case_equal(request->connection, "close");
} else {
    // 1.0: đóng TRỪ khi có "keep-alive"
    request->keep_alive_requested = ascii_case_equal(request->connection, "keep-alive");
}
```

---

## Luồng xử lý request

```
Client gửi request line → parse method, URI, version
                              ↓
                    parse headers (Connection, Range)
                              ↓
                    Xác định keep-alive có được giữ không?
                              ↓
                    Dispatch: file / directory / error
                              ↓
                    Ghi access log
                              ↓
                    Lặp lại nếu keep-alive, đóng nếu không
```

### Các bước chính

1. **Tìm CRLF** (`\r\n`) để xác định request line
2. **Tách method, URI, version** bằng dấu space
3. **Strip query string** khỏi URI (lấy phần trước `?`)
4. **Parse headers**: `Connection` và `Range` được extract
5. **Xác định keep-alive** dựa trên version + Connection header
6. **Serve file** hoặc trả error (400, 403, 404, 416, 501)

---

## Các điểm quan trọng cần nhớ

### 1. Keep-Alive logic

- **HTTP/1.1**: Keep-alive mặc định → trừ khi client gửi `Connection: close`
- **HTTP/1.0**: Đóng mặc định → trừ khi client gửi `Connection: keep-alive`

### 2. Method support

- Chỉ hỗ trợ `GET` và `HEAD`
- Method khác (POST, PUT, DELETE...) → trả **501 Not Implemented**
- Request line không hợp lệ → trả **400 Bad Request**

### 3. Range request

Hỗ trợ hai dạng:
- `bytes=100-199` → lấy byte từ 100 đến 199
- `bytes=-100` → lấy 100 byte cuối (suffix range)

Multi-range (có dấu phẩy) → không hỗ trợ, bị bỏ qua.

### 4. Query string

URI như `/page?id=5` → chỉ lưu `/page`, phần `?id=5` bị bỏ.

### 5. Buffered request cho Keep-Alive

Server dùng một buffer cố định, sau khi parse xong một request, dữ liệu còn lại (nếu có) được giữ lại trong buffer để parse request tiếp theo mà không cần thêm `recv()`.

### 6. HTTP status codes

| Code | Khi nào |
|------|---------|
| 200 | Serve file thành công |
| 206 | Range request thành công |
| 400 | Request line sai định dạng |
| 403 | Path traversal bị chặn |
| 404 | File/directory không tồn tại |
| 416 | Range không hợp lệ |
| 501 | Method không hỗ trợ |
| 503 | Queue đầy (backpressure) |

---

## Câu hỏi thường gặp trong phỏng vấn

**Q: Tại sao HTTP/1.0 và HTTP/1.1 xử lý Connection header khác nhau?**

A: HTTP/1.0 được thiết kế mặc định đóng kết nối sau mỗi response vì mỗi request/response là một round-trip đơn lẻ. HTTP/1.1 cải tiến với keep-alive mặc định để giảm overhead của việc thiết lập TCP connection mới. Đây là backward-compatible design: server cũ vẫn hoạt động với client mới.

**Q: Buffer size cho request line là bao nhiêu?**

A: 256 bytes (`request_line[256]` tại `src/server.c:404`). Nếu request line dài hơn, server coi đó là malformed và trả 400.

**Q: Server có hỗ trợ HTTP pipelining?**

A: Có, một phần. Buffer giữ lại dữ liệu sau request đầu tiên, nhưng multi-range Range header không được hỗ trợ.

**Q: Keep-Alive connection có timeout không?**

A: Trong implementation này, không có explicit timeout. Kết nối chỉ đóng khi client gửi `Connection: close`, client đóng kết nối, hoặc server bị shutdown.
