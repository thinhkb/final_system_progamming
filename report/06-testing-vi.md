# Testing & Benchmarking — Vietnamese Interview Summary

**Phase:** 06 | **Tóm tắt:** Hạ tầng test đầy đủ: unit test + integration test + benchmark

---

## Chiến lược Test

Dự án dùng **3 tầng test** theo hình kim tự tháp:

```
       ┌──────────────────┐
       │    Benchmark     │  ← bench.sh: 120 clients đồng thời
       │ ┌──────────────┐ │
       │ │ Integration  │ │  ← run_tests.sh: server thật + curl
       │ │  (20+ test)  │ │
       │ └──────────────┘ │
       │ ┌──────────────┐ │
       │ │    Unit      │ │  ← unit_*.c: 16 tests, nhanh, cô lập
       │ └──────────────┘ │
       └──────────────────┘
```

**Triết lý:**
- **Unit test**: test từng function riêng lẻ, không phụ thuộc bên ngoài
- **Integration test**: test toàn bộ server end-to-end (HTTP requests thật)
- **Benchmark**: đo performance dưới tải nặng (120 clients)

---

## Các Test Cases Quan trọng

### Unit Tests (tests/unit_*.c)

| Test | Mục đích |
|------|----------|
| `test_parse_get_http11` | HTTP/1.1 GET → keep-alive mặc định |
| `test_connection_close_disables_keep_alive` | Connection: close override keep-alive |
| `test_parse_unsupported_method` | POST → 501 Not Implemented |
| `test_parse_explicit_range` | Range: bytes=7-10 → parsed đúng |
| `test_parse_suffix_range` | Range: bytes=-5 → nhận biết suffix range |
| `test_mime_types` | .html → text/html, .css → text/css, unknown → application/octet-stream |
| `test_reject_traversal` | /../etc/passwd → FORBIDDEN (bảo mật) |
| `test_directory_listing_url_encodes_links` | space name → space%20name trong href |
| `test_fifo_and_full` | Queue FIFO, full → QUEUE_FULL |
| `test_shutdown_wakes_consumer` | Consumer blocked được wake khi shutdown |

### Integration Tests (tests/run_tests.sh)

| Test | curl command | Verify |
|------|-------------|--------|
| 1 | `curl http://.../index.html` | 200 OK, content đúng |
| 2 | `curl "http://...?cache=false"` | Query string bị strip |
| 3 | `curl -I http://.../about.txt` | HEAD → 200, không có body |
| 4 | `curl http://.../missing.txt` | 404 Not Found |
| 5 | `curl http://.../../Makefile` | 403 Forbidden (path traversal) |
| 6 | `curl http://.../listing/` | Directory listing HTML |
| 7 | `curl "http://.../space%20name.txt"` | URL-encoded file retrieval |
| 8 | `curl -X POST http://.../` | 501 Not Implemented |
| 9 | `curl -H "Range: bytes=0-5" http://.../` | 206 Partial Content |
| 10 | `curl -H "Range: bytes=9999-10000" http://.../` | 416 Range Not Satisfiable |
| 11 | netcat two requests over one connection | Keep-Alive hoạt động |
| 12 | Access log grep | Common Log Format được ghi |

---

## Keep-Alive Test với netcat

**Tại sao dùng netcat thay vì curl?**

curl mặc định mở connection mới cho mỗi request. netcat cho phép gửi **nhiều request qua 1 TCP connection**:

```bash
# Gửi 2 request liên tiếp qua 1 socket
(printf "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"
 printf "GET /about.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n") | \
nc -w 3 127.0.0.1 18080 > /tmp/output.txt

# Kiểm tra cả 2 response đều có mặt trong output
grep -q "HTTP/1.1 200 OK" /tmp/output.txt
```

---

## Benchmark: 120 Concurrent Clients

**File:** `bench/bench.sh`

### Cách hoạt động

```bash
CLIENTS=120

# Spawn 120 background subshells, mỗi cái chạy 1 curl
for i in $(seq 1 $CLIENTS); do
    (curl --max-time 5 -s -o /dev/null http://... &
     echo $? > /tmp/bench_result_${i}.txt) &
done

wait  # Đợi tất cả clients xong
```

### Kết quả đo được

| Metric | Công thức |
|--------|-----------|
| Successes | `count(exit_code == 0)` |
| Failures | `count(exit_code != 0)` — phải = 0 mới pass |
| Elapsed (ms) | `END - START` (wall-clock time) |
| **Requests/second** | `CLIENTS × 1000 / ELAPSED_MS` |

---

## Makefile Targets

| Command | Làm gì |
|---------|--------|
| `make test-unit` | Build và chạy 3 unit test binaries riêng biệt |
| `make test-integration` | Build httpd, chạy run_tests.sh |
| `make test` | Chạy cả unit + integration |
| `make bench` | Build → start server → chạy bench.sh → kill server |

```bash
# Chạy tất cả test
make test

# Benchmark
make bench
```

---

## Key Takeaways

1. **16 unit tests** + **20+ integration assertions** + **benchmark 120 clients**
2. **Custom test framework** (không dùng CUnit) — zero dependency, đủ dùng
3. **run_tests.sh** dùng `trap` để cleanup server dù test có fail hay không
4. **netcat** để test Keep-Alive (curl không hỗ trợ multi-request trên 1 connection dễ dàng)
5. **Benchmark** đo throughput = requests/second, pass khi failures = 0
6. **`make test`** = chạy tất cả, **`make bench`** = đo performance

---

## Câu hỏi phỏng vấn hay gặp

**Q: Tại sao dùng temp files để collect benchmark results?**
A: Background subshells chạy song song, không share memory. Mỗi subshell ghi exit code vào file riêng. Parent process đọc sau khi `wait`.

**Q: Tại sao cần `make clean` giữa các benchmark runs?**
A: Stale object files có thể affect stability. Clean build đảm bảo kết quả nhất quán.

**Q: `test_shutdown_wakes_consumer` test cái gì?**
A: Consumer thread đang blocked ở `dequeue_task` (không có task). Khi `shutdown_thread_pool` được gọi, consumer phải được wake lên và exit gracefully — không deadlock.

**Q: Sự khác nhau giữa test-integration và benchmark?**
A: Integration test check **correctness** (status codes, headers, content). Benchmark check **performance** (throughput, concurrent handling). Integration test pass/fail. Benchmark pass = 0 failures + measurable RPS.
