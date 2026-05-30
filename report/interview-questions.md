# Interview Questions — Multi-Threaded HTTP File Server

This document contains expected interview questions from the teacher, organized by phase. Each question is paired with a concise Vietnamese answer (for oral delivery during the interview) and a brief English explanation (for deeper understanding).

---

## Phase 1: HTTP Protocol & Keep-Alive

### Q1: What is the difference between HTTP/1.0 and HTTP/1.1 Keep-Alive behavior?

**VI:** HTTP/1.0 mặc định đóng kết nối sau mỗi response. Muốn keep-alive phải gửi header `Connection: keep-alive`. HTTP/1.1 ngược lại — mặc định giữ kết nối, muốn đóng phải gửi `Connection: close`. Đây là cách HTTP/1.1 cải thiện hiệu năng, giảm overhead của việc thiết lập TCP connection mới.

**EN:** The key difference is in the defaults. HTTP/1.0 closes the connection after each response unless `Connection: keep-alive` is sent. HTTP/1.1 keeps the connection alive by default unless `Connection: close` is sent. This is implemented in `src/http.c:266–270` where the version-specific logic sets `keep_alive_requested` accordingly.

---

### Q2: Why does the server return 501 for POST but 400 for malformed requests?

**VI:** 501 (Not Implemented) nghĩa là request đúng format nhưng server không hỗ trợ method đó. POST, PUT, DELETE là valid HTTP method nhưng server chỉ implement GET và HEAD. Còn 400 (Bad Request) nghĩa là request sai format — thiếu version, thiếu CRLF, hoặc thiếu thành phần nào đó trong request line.

**EN:** 501 means "I understand your request format but cannot handle this method" — POST is syntactically valid HTTP but unsupported. 400 means "I cannot even understand your request" — the request line is malformed. See `src/http.c` for parsing validation and `src/server.c:435` for the 501 response.

---

### Q3: How does the server handle Keep-Alive connections internally?

**VI:** Server dùng một static buffer cố định để đọc request. Sau khi parse xong request đầu tiên, nếu keep-alive được kích hoạt, dữ liệu còn lại (từ pipelined request) được giữ trong buffer — không cần gọi thêm `recv()`. Luồng: `recv()` vào buffer → parse request → `memmove()` giữ lại phần thừa → lặp lại.

**EN:** The server maintains a per-connection buffer (`src/server.c:403`). After parsing one request, any remaining buffered data (from HTTP pipelining) is kept with `memmove()` so the next request can be parsed immediately without another `recv()` call. See `src/server.c:113–114`.

---

### Q4: What happens if the request line is longer than 256 bytes?

**VI:** Server có `request_line[256]` — nếu request line dài hơn 256 bytes, nó bị truncate trước khi parse. Điều này có thể dẫn đến parse thất bại và trả về 400 Bad Request. Buffer overflow được ngăn chặn bởi vì code giới hạn độ dài trước khi copy.

**EN:** The `request_line` buffer in `src/server.c:404` is 256 bytes. A longer request line gets truncated during extraction, which typically causes the parser to fail and return 400 Bad Request. Buffer overflow is prevented by bounded copy operations throughout the parser.

---

## Phase 2: Thread Pool Architecture

### Q5: Why use a fixed thread pool instead of creating a new thread per connection?

**VI:** Tạo thread mới cho mỗi connection tốn chi phí cao — mỗi request đều phải gọi `pthread_create()`. Với 1000 concurrent connections, sẽ có 1000 threads → hết bộ nhớ (OOM), quá nhiều context switch. Fixed pool giới hạn số lượng thread đồng thời, chỉ tạo thread một lần lúc khởi động. Queue có giới hạn tạo backpressure khi system quá tải.

**EN:** Thread-per-connection incurs `pthread_create()` overhead per request and can exhaust memory under load. A fixed pool pre-creates threads once at startup, limits concurrency, and the bounded queue provides backpressure. See the comparison table in `report/02-thread-pool-en.md` for the full trade-off analysis.

---

### Q6: Explain the producer-consumer pattern in this server.

**VI:** Acceptor thread (trong `server_run()`) đóng vai trò producer — nó nhận kết nối TCP từ `accept()` và đặt `client_fd` vào queue. Các worker threads đóng vai trò consumer — chúng lấy `client_fd` từ queue bằng `dequeue()` và xử lý HTTP request. Queue là cầu nối giữa hai bên.

**EN:** The acceptor thread (producer) calls `accept()` and enqueues client file descriptors. Worker threads (consumers) dequeue them and process requests. This decouples connection acceptance from request processing and naturally distributes load via FIFO ordering. See `src/server.c:557–572` for the producer and `src/thread_pool.c:22–34` for the consumer loop.

---

### Q7: How does the bounded queue handle synchronization?

**VI:** Queue dùng `pthread_mutex_t` để bảo vệ truy cập shared state (count, head, tail). Hai condition variables: `not_empty` được signal khi có item mới (đánh thức worker đang chờ), `not_full` được signal khi có chỗ trống. Enqueue không block — nếu queue đầy thì trả `QUEUE_FULL` ngay. Dequeue block — worker chờ trên `pthread_cond_wait()` nếu queue rỗng.

**EN:** The queue uses `pthread_mutex_t` for mutual exclusion and two condition variables (`not_empty`, `not_full`) for signaling. Enqueue is non-blocking (returns `QUEUE_FULL` immediately when at capacity). Dequeue is blocking (waits on `pthread_cond_wait()` when the queue is empty). The circular buffer wraps head/tail indices with modulo arithmetic. See `src/thread_pool.c:90–147`.

---

### Q8: Why use `while` instead of `if` when waiting on a condition variable?

**VI:** POSIX cho phép **spurious wakeup** — thread có thể được wakeup mà không có signal thật sự. Dùng `while` đảm bảo thread kiểm tra lại điều kiện sau khi wakeup. Nếu dùng `if`, thread có thể tiếp tục khi queue vẫn rỗng → bug.

**EN:** `pthread_cond_wait` can wake spuriously (without an actual signal). Using `while` ensures the condition is re-checked after waking, preventing the worker from proceeding when the queue is still empty. This is the standard correct pattern. See `src/thread_pool.c:191`.

---

### Q9: How does graceful shutdown work?

**VI:** Khi `server_stop()` được gọi: (1) set `should_stop = 1`, (2) close listening socket để không accept nữa, (3) gọi `socket_queue_shutdown()` để set shutdown flag và broadcast cả hai condition variables. Workers đang chờ trên queue rỗng được wake lên, thấy shutdown flag, trả `QUEUE_CLOSED` và exit. Main thread join tất cả workers trước khi free resources.

**EN:** Shutdown sets `should_stop = 1`, closes the listening socket, then calls `socket_queue_shutdown()` which sets the shutdown flag and broadcasts both CVs. Waiting workers wake up, find the empty queue with shutdown set, return `QUEUE_CLOSED`, and exit. The main thread `pthread_join`s all workers before freeing resources. See `src/thread_pool.c:186–202`.

---

### Q10: What is the deadlock risk in this design?

**VI:** Không có deadlock potential. Acceptor không bao giờ chờ worker — nó chỉ enqueue. Worker chỉ giữ mutex trong thời gian rất ngắn (lấy/đặt item vào queue), rồi xử lý request bên ngoài mutex. Thứ tự lock luôn nhất quán: mutex → thao tác queue → unlock.

**EN:** There is no deadlock risk. The acceptor never waits for a worker — it only enqueues. Workers hold the mutex only briefly during queue operations, then release it before processing. The lock order is consistent (mutex only) and workers never call back into shared code while holding it.

---

## Phase 3: File System & Security

### Q11: Describe the 5-step path resolution pipeline.

**VI:** (1) **URL decode** — giải mã `%XX` sequences và `+` → space. (2) **Reject `..`** — kiểm tra string trước khi gọi filesystem. (3) **Join path** — nối doc_root + path bằng `snprintf`. (4) **realpath** — resolve cả doc_root và target thành canonical absolute paths. (5) **Chroot check** — verify canonical path bắt đầu bằng doc_root bằng `strncmp`.

**EN:** (1) URL decode %XX and +. (2) Reject `..` segments at string level. (3) Join doc_root + path with snprintf. (4) Call `realpath()` on both doc_root and target. (5) Verify the resolved path starts with the doc_root via `strncmp`. See `src/files.c:221–363` for the full pipeline.

---

### Q12: Why check for `..` at the string level before calling `realpath`?

**VI:** Vì `realpath()` trên một số hệ thống có thể resolve symlink `..` thành path bên ngoài doc_root. Kiểm tra ở string level là lớp phòng thủ độc lập — nếu có bug trong `realpath` hoặc trong filesystem, `..` rejection vẫn hoạt động.

**EN:** Because `realpath()` on some systems may resolve symlinked `..` segments to paths outside the document root. String-level rejection before `realpath()` is a defense-in-depth measure that protects even if the filesystem layer has unexpected behavior.

---

### Q13: Why do you call `realpath()` on both the doc_root and the target path?

**VI:** Nếu chỉ resolve doc_root, attacker có thể exploit symlink trong doc_root. Nếu chỉ resolve target, không có baseline để so sánh. Resolve cả hai đảm bảo canonical paths đáng tin cậy từ cả hai phía.

**EN:** If only the target is resolved, there is no baseline to compare against. If only the doc_root is resolved, a symlink inside the doc_root could escape. Resolving both ensures canonical paths from both sides, and the `strncmp` check validates the final path is within the canonical doc_root.

---

### Q14: Why do directory listings need both HTML escaping and URL encoding?

**VI:** Hai ngữ cảnh khác nhau. **HTML escaping** cho phần hiển thị: tên file như `<script>` phải thành `&lt;script&gt;` để browser hiển thị text thay vì chạy JS. **URL encoding** cho phần `href`: khoảng trắng trong URL phải thành `%20` để click hoạt động đúng. Một file `A & B <test>` cần cả hai: hiển thị `A &amp; B &lt;test&gt;`, link `A%20%26%20B%20%3Ctest%3E`.

**EN:** HTML escaping is for display inside the `<body>` — prevents XSS and rendering issues. URL encoding is for the `href` attribute — spaces are illegal in URLs and must become `%20`. They serve completely different encoding contexts and both are necessary. See `src/files.c:91–149` for both implementations.

---

### Q15: What happens if `readdir()` returns `DT_UNKNOWN` for an entry's type?

**VI:** `readdir()` có thể trả `DT_UNKNOWN` trên một số filesystem (NFS, network filesystems). Code gọi `stat()` trên mỗi entry để xác định type chắc chắn — directory hay file. Không làm điều này, directory listing có thể hiển thị sai loại entry.

**EN:** `d_type` may be `DT_UNKNOWN` on network filesystems. The code calls `stat(2)` on each entry to reliably determine its type before deciding whether to append a trailing `/` in the listing. See `src/files.c:432–435`.

---

## Phase 4: Access Logging

### Q16: Explain the Common Log Format fields this server logs.

**VI:** Format: `IP - - [timestamp] "request line" status bytes`. Hai dấu `-` đầu tiên là placeholders cho ident (RFC 931) và authuser (authentication) — cả hai đều không implement. Timestamp theo format CLF: `dd/Mon/yyyy:HH:mm:ss +zzzz`.

**EN:** The format is `host ident authuser [timestamp] "request" status bytes`. This server logs `127.0.0.1 - - [10/Oct/2026:13:55:36 +0000] "GET /index.html HTTP/1.1" 200 232`. The two dashes are intentional placeholders — ident lookup requires an ident daemon, and authentication is not implemented. See `src/log.c:64–66`.

---

### Q17: Why is a mutex needed for access logging?

**VI:** Server dùng thread pool — nhiều worker threads xử lý request song song. Nếu hai thread gọi `fprintf()` cùng lúc mà không có mutex, các dòng log bị chen lẫn nhau, không đọc được. Mutex đảm bảo chỉ một thread ghi log tại một thời điểm.

**EN:** Multiple worker threads can call `access_log_write()` simultaneously. Without a mutex, concurrent `fprintf()` calls produce interleaved, garbled log lines — a data race on the FILE stream. The mutex serializes writes. See `src/log.c:64–67`.

---

### Q18: Why is `fflush()` called inside the mutex?

**VI:** Output trong C được buffered — `fprintf` không ghi ngay xuống đĩa mà chỉ copy vào buffer. Nếu process crash trước khi buffer đầy, dòng log cuối cùng bị mất. `fflush()` bên trong mutex đảm bảo mỗi dòng log được flush xuống kernel buffer trước khi unlock — ít mất log hơn khi crash.

**EN:** C output is buffered. `fprintf` copies data to a kernel buffer and returns immediately. If the process crashes before the buffer is flushed, the last log entries are lost. `fflush()` inside the mutex guarantees each log line reaches the kernel buffer before the lock is released, minimizing log loss on crash. See `src/log.c:67`.

---

### Q19: Why use `localtime_r` instead of `localtime`?

**VI:** `localtime()` dùng static internal buffer — nếu nhiều threads gọi cùng lúc, timestamp có thể bị chen lẫn. `localtime_r()` dùng buffer do caller cung cấp trên stack — thread-safe, mỗi thread có timestamp riêng.

**EN:** `localtime()` uses a shared static buffer internally, which is not thread-safe. Multiple threads calling it simultaneously would corrupt the buffer. `localtime_r()` writes to a caller-supplied stack buffer, making it safe for concurrent calls from multiple threads.

---

## Phase 5: Range Requests

### Q20: What is the difference between HTTP 206 and HTTP 416?

**VI:** **206 Partial Content** — range hợp lệ, server gửi một phần của file. Response có `Content-Range: bytes start-end/total` và `Content-Length` bằng kích thước range. **416 Range Not Satisfiable** — range không thể satisfy được (ví dụ: yêu cầu bytes 9999-10000 cho file 100 bytes). Response có `Content-Range: bytes */total` — dấu `*` thay vì range, kèm kích thước file để client tính toán lại.

**EN:** 206 means "your range is valid, here is the portion you asked for." 416 means "your range is syntactically correct but cannot be satisfied." The 416 response must include `Content-Range: bytes */size` per RFC 9110, telling the client the actual file size so it can compute valid ranges. See `src/server.c:225–257` for the 416 implementation.

---

### Q21: How does the server handle suffix ranges like `bytes=-5`?

**VI:** Suffix range `bytes=-5` nghĩa là "5 bytes cuối cùng của file." Server parse suffix length = 5. Khi resolve, nếu suffix length ≤ file size → start = file_size - 5. Nếu suffix length > file size → clamp start = 0, trả về toàn bộ file với HTTP 200 (không phải 416). Code xử lý ở `src/server.c:202–205`.

**EN:** `bytes=-5` means "last 5 bytes." The parser stores the suffix length in `range_start`. During resolution, if suffix ≤ file size, start = file_size - suffix. If suffix > file size, start = 0 and a full 200 response is returned (not 416). See `src/server.c:202–223`.

---

### Q22: Why use `fseeko` instead of `fseek`?

**VI:** `fseek()` dùng offset kiểu `long` (32-bit) — tràn với file lớn hơn 2GB trên hệ thống 32-bit. `fseeko()` dùng offset `off_t` (64-bit trên modern systems) — hỗ trợ file lớn trên mọi platform. HTTP server nên dùng `fseeko()` để an toàn.

**EN:** `fseek()` uses a 32-bit `long` offset, which overflows for files larger than 2GB on 32-bit systems. `fseeko()` uses `off_t` (64-bit on modern systems), making it compatible with large files on all platforms. See `src/server.c:278`.

---

### Q23: Why does the server send `Accept-Ranges: bytes` on all file responses, not just 206?

**VI:** Gửi trong cả 200 và 206 giúp client biết server có hỗ trợ range requests trước khi thử — không cần guess. RFC 9110 khuyến nghị server advertise range capability trong mọi response hợp lệ. Implementation đơn giản hơn — luôn gửi thay vì phải track state.

**EN:** Advertising range support in all responses (200 and 206) lets clients discover capability without guessing. RFC 9110 recommends this. It also simplifies the implementation — always include it rather than tracking state conditionally. See `src/server.c:301–306`.

---

## Phase 6: Testing & Benchmarking

### Q24: Why is the Keep-Alive integration test written with netcat instead of curl?

**VI:** curl mặc định mở TCP connection mới cho mỗi request. netcat (`nc`) cho phép gửi nhiều HTTP requests liên tiếp qua **một** TCP connection — đúng cách Keep-Alive hoạt động. Test gửi hai requests qua một socket, kiểm tra cả hai responses đều có mặt.

**EN:** curl opens a new connection per request by default. netcat gives precise socket control — it can send multiple HTTP requests over a single TCP connection, exactly how Keep-Alive works. The test sends two pipelined requests and verifies both responses appear. See `tests/run_tests.sh:60–66`.

---

### Q25: What does the benchmark actually measure?

**VI:** Benchmark đo throughput: bao nhiêu requests thành công trong bao lâu. Công thức: `requests_per_second = clients × 1000 / elapsed_ms`. Nó spawn 120 background curl subshells đồng thời, đo wall-clock time từ lúc bắt đầu đến khi tất cả hoàn thành. Pass khi `failures == 0` và RPS có thể đo được.

**EN:** The benchmark measures throughput under concurrent load. 120 background curl subshells run simultaneously, wall-clock time is measured, and requests/second is calculated. The test asserts `failures == 0` — any failed request fails the benchmark. See `bench/bench.sh:15–38`.

---

### Q26: How do unit tests and integration tests differ in this project?

**VI:** **Unit tests** test từng module riêng lẻ, không có network, không có server thật. Ví dụ: chỉ test `http_parse_request()` với input string. Fast, deterministic, isolated. **Integration tests** chạy server thật trên port thật, gửi curl requests thật, kiểm tra responses thật — bao gồm cả HTTP semantics, status codes, và end-to-end flow.

**EN:** Unit tests exercise individual modules in isolation without networking — e.g., calling `http_parse_request()` with a raw string. Fast, deterministic, no dependencies. Integration tests start a real server, send real curl requests, verify real responses — covering HTTP semantics, status codes, and end-to-end behavior.

---

## Cross-Cutting Questions

### Q27: Walk me through what happens when a client sends a complete HTTP request.

**VI:** (1) TCP connection được accept bởi acceptor thread. (2) `client_fd` được enqueue vào bounded queue. (3) Worker dequeue `client_fd`, gọi `recv()` đọc request vào buffer. (4) HTTP parser tách method, URI, version, headers (Connection, Range). (5) File resolver decode URL, kiểm tra path traversal, resolve file. (6) Response được gửi: headers + body (hoặc 206/416 cho range). (7) Access log được ghi. (8) Nếu Keep-Alive → quay lại bước 3, không thì đóng socket.

**EN:** The full flow: accept → enqueue → dequeue → recv → parse HTTP → resolve path → send response → log → (optionally loop for Keep-Alive). Each step maps to a specific module: `server.c` for accept/enqueue, `thread_pool.c` for dequeue, `http.c` for parsing, `files.c` for path resolution, `server.c` for response, `log.c` for logging.

---

### Q28: What are the main performance bottlenecks of this server?

**VI:** (1) **File I/O** — reading files from disk, đặc biệt với file lớn. (2) **Access log mutex** — mỗi response phải lock/unlock để ghi log. (3) **Keep-Alive với idle connections** — worker bị chiếm trong khi client giữ connection mở dù không làm gì. (4) **Queue contention** — nhiều workers cùng tranh mutex queue. Cải tiến tiềm năng: `sendfile()` thay vì `fread()+send()`, per-worker log buffer, event-driven cho Keep-Alive idle connections.

**EN:** The main bottlenecks are file I/O (disk reads), the access log mutex (every response locks), Keep-Alive with idle connections (workers tied up), and queue contention. Future optimizations include `sendfile()` for zero-copy transfer, per-worker log buffering, and an event-driven architecture for idle Keep-Alive connections.

---

### Q29: How does the server handle the case where all worker threads are busy?

**VI:** Workers tiếp tục xử lý requests đang có. Queue tích lũy thêm items. Nếu queue đầy (đạt capacity) → `enqueue()` trả `QUEUE_FULL` → acceptor gửi HTTP 503 Service Unavailable và close socket ngay. Client nhận 503 biết server đang quá tải và có thể retry sau. Đây là **backpressure** — cách system báo cho upstream biết nó không thể handle thêm.

**EN:** Workers continue processing their current requests. The queue accumulates more items. If the queue is full (at capacity), `enqueue()` returns `QUEUE_FULL`, the acceptor sends HTTP 503 and closes the socket immediately. This is backpressure — the system signals to clients that it is overloaded. See `src/server.c:571–578`.

---

### Q30: Describe a scenario where the security measures could still be bypassed.

**VI:** Nếu doc_root là một symlink trỏ đến `/tmp`, attacker request `/../../../../../tmp/secret.txt`. Sau realpath, canonical path vẫn bắt đầu bằng `/tmp` (nơi symlink resolve đến), không phải doc_root. Nhưng vì `/tmp` được resolved từ symlink — nó sẽ được resolve thành canonical `/tmp`, và check `strncmp` sẽ pass vì nó nằm trong doc_root. Tuy nhiên đây là behavior đúng theo design — symlink nằm trong doc_root được phép truy cập. Lỗ hổng thực sự có thể xảy ra nếu filesystem có race condition (symlink race) giữa `realpath` check và `stat/open` — cần `openat()` với `O_NOFOLLOW` hoặc dùng `O_PATH` để fix.

**EN:** If doc_root itself is a symlink, `realpath()` resolves it to its target. A path that resolves inside the symlink target will pass the `strncmp` check even if it wouldn't be reachable through the actual doc_root path. Additionally, a TOCTOU (time-of-check-time-of-use) race between `stat()` and `fopen()` could theoretically allow a symlink to be swapped between the check and the open. The proper fix is using `openat()` with `O_NOFOLLOW` or `O_PATH` to atomically handle symlinks.
