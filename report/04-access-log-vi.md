# Access Logging — Báo Cáo Kỹ Thuật (Tiếng Việt)

## Tổng Quan

Module `src/log.c` ghi lại mọi request đã xử lý vào file log theo định dạng Common Log Format (CLF). Mỗi dòng log chứa IP client, thời gian, request line, status code, và số bytes gửi. Phần quan trọng nhất của implementation là thread-safety — nhiều worker thread có thể ghi log cùng lúc, và nếu không đồng bộ hóa, các dòng log sẽ bị chen lẫn nhau.

---

## 1. Common Log Format (CLF)

Định dạng chuẩn từ NCSA httpd, mỗi dòng có cấu trúc:

```
host ident authuser [timestamp] "request" status bytes
```

Server này bỏ qua 2 trường `ident` và `authuser`, nên format thực tế:

```
127.0.0.1 - - [10/Oct/2026:13:55:36 +0000] "GET /index.html HTTP/1.1" 200 232
^^^^^^^^  ^ ^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ~~~~~~~~~~~~~~~~~~~~~~~~~ ^^ ~~
   IP     | |          timestamp                    request           |  |
      ident (không dùng)                        status              bytes
  authuser (không dùng)
```

Hai dấu gạch ngang `-` là placeholder, không phải lỗi.

---

## 2. Timestamp Format

CLF yêu cầu format cố định, khác ISO 8600:

```
dd/Mon/yyyy:HH:mm:ss +zzzz
10/Oct/2026:13:55:36 +0000
```

```c
// src/log.c — Timestamp formatting
char timestamp[64];
time_t now = time(NULL);
struct tm tm_buf;
localtime_r(&now, &tm_buf);         // Thread-safe variant
strftime(timestamp, sizeof(timestamp),
         "%d/%b/%Y:%H:%M:%S %z", &tm_buf);
```

`localtime_r` là phiên bản thread-safe của `localtime`. `localtime` thường dùng static buffer nội bộ → race condition khi nhiều thread gọi đồng thời.

---

## 3. Thread-Safety — Tại Sao Cần Mutex

Server dùng thread pool: nhiều worker thread xử lý request song song. Khi hai thread gọi `access_log_write()` cùng lúc mà không có mutex:

```
Thread A: fprintf("127.0.0.1 - ...")
Thread B: fprintf("192.168.1.1 - ...")
→ Kết quả: dòng log bị chen lẫn, không đọc được
```

**Giải pháp:**

```c
// src/log.c — Mutex-protected write
pthread_mutex_lock(&log->mutex);
fprintf(log->file,
        "%s - - [%s] \"%s\" %d %zu\n",
        client_ip, timestamp, request_line, status_code, bytes_sent);
fflush(log->file);                   // Quan trọng!
pthread_mutex_unlock(&log->mutex);
```

Mutex đảm bảo chỉ một thread ghi log tại một thời điểm.

---

## 4. Tại Sao Cần `fflush`?

Output trong C được **buffered** — `fprintf` không ghi ngay xuống đĩa, mà chỉ copy vào kernel buffer. Nếu process crash trước khi buffer đầy:

```
Crash! → Buffer chưa kịp ghi → Mất 1-2 dòng log cuối
```

`fflush(log->file)` bên trong mutex → **mỗi dòng log được đảm bảo ghi xuống kernel buffer trước khi release mutex**. Ít mất log hơn khi crash.

**Đánh đổi:** `fflush` là syscall → tốn hiệu năng. Với server tải cao, đây có thể là bottleneck. Alternative: dùng line-buffered mode (`setvbuf(..., _IOLBF, 0)`) — flush tự động tại mỗi `\n` — nhưng an toàn hơn một chút so với fully-buffered.

---

## 5. Những Thứ Được Log

| Trường       | Ví dụ giá trị             | Ý nghĩa                           |
|--------------|---------------------------|-----------------------------------|
| `client_ip`  | `192.168.1.100`           | IP của client (TCP connection)    |
| `timestamp`  | `[10/Oct/2026:13:55 +0000]` | Thời gian server nhận request |
| `request_line` | `"GET /index.html HTTP/1.1"` | Method + URI + Version      |
| `status_code` | `200`, `404`, `500`       | HTTP status của response          |
| `bytes_sent`  | `232`, `0`               | Số bytes body gửi đi             |

**Quy tắc bytes_sent:**
- `0` cho các response lỗi (400, 403, 404, 500) — không gửi body
- File rỗng → log `0` cho 200 OK → đúng
- 304 Not Modified → có thể là `0` hoặc size của file

---

## 6. Thứ Tự Gọi Log

```
Request → Parse → Validate → Send Response
                                     ↓
                          access_log_write() ← Gọi SAU khi gửi xong
                                     ↓
                          Close connection
```

Log sau khi response hoàn tất → ghi đúng outcome, không phải request.

---

## Key Takeaways

1. **CLF format**: IP `- - [timestamp] "request" status bytes`
2. **2 dấu `-`**: ident và authuser không implement
3. **Mutex**: nhiều thread ghi log đồng thời → race condition → mutex bắt buộc
4. **fflush**: đảm bảo dòng log được ghi xuống buffer kernel → ít mất log khi crash
5. **localtime_r**: thread-safe, dùng buffer trên stack, không dùng static buffer
6. **%zu format**: đúng cho `size_t` trên cả 32-bit và 64-bit
