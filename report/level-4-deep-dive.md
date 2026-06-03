# Level 4 Deep Dive — Concurrency Patterns

Level 4 là nơi các khối xây dựng từ Level 2 và Level 3 kết hợp với nhau thành các **mẫu thiết kế concurrency** (concurrency design patterns). Đây là các giải pháp đã được chứng minh cho các bài toán đồng thời kinh điển, và project này triển khai tất cả chúng.

---

## Mục lục

1. [Tổng quan 5 patterns và mối quan hệ](#1-tổng-quan-5-patterns-và-mối-quan-hệ)
2. [Producer-Consumer — Mẫu nền tảng](#2-producer-consumer--mẫu-nền-tảng)
3. [Bounded Queue — Giải pháp hữu hạn](#3-bounded-queue--giải-pháp-hữu-hạn)
4. [Backpressure — Từ chối có kiểm soát](#4-backpressure--từ-chối-có-kiểm-soát)
5. [Thread Pool — Tái sử dụng luồng](#5-thread-pool--tái-sử-dụng-luồng)
6. [Spurious Wakeups — Wake lên mà không có lý do](#6-spurious-wakeups--wake-lên-mà-không-có-lý-do)
7. [Tích hợp toàn bộ hệ thống](#7-tích-hợp-toàn-bộ-hệ-thống)
8. [Phân tích sâu: Tại sao thiết kế này đúng](#8-phân-tích-sâu-tại-sao-thiết-kế-này-đúng)

---

## 1. Tổng quan 5 patterns và mối quan hệ

### 1.1 Dependency graph

```
┌──────────────────────────────────────────────────────────────────┐
│                                                                  │
│  ┌──────────────────────┐   Producer    ┌──────────────────┐ │
│  │   TCP Socket Layer   │ ───────────────→ │  Bounded Queue   │ │
│  │   (Level 2)         │                 │  (Circular Buf)  │ │
│  └──────────────────────┘                 └────────┬─────────┘ │
│                                                      │           │
│                                                      │ dequeue  │
│                                                      ↓           │
│  ┌──────────────────────┐   Consumer    ┌──────────────────┐ │
│  │   HTTP + Filesystem   │ ←────────────── │   Thread Pool    │ │
│  │   (Level 5+6)        │                 │   (N workers)   │ │
│  └──────────────────────┘                 └────────┬─────────┘ │
│                                                      │           │
│                     BACKPRESSURE                        │           │
│                     ←──────────────────────────────────────────────┤
│                     QUEUE FULL → 503                     │           │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 Mỗi pattern giải quyết gì?

| Pattern | Bài toán | Giải pháp |
|---------|---------|----------|
| **Producer-Consumer** | Một bên tạo work, bên kia xử lý | Tách biệt production và consumption qua queue |
| **Bounded Queue** | Work tích lũy vô hạn | Giới hạn buffer → backpressure |
| **Backpressure** | Hệ thống quá tải | Từ chối có kiểm soát (503) |
| **Thread Pool** | Tạo thread mới cho mỗi job đắt đỏ | Pre-create N threads, tái sử dụng |
| **Spurious Wakeups** | CV wake mà không có signal thật | Dùng `while` thay vì `if` |

---

## 2. Producer-Consumer — Mẫu nền tảng

### 2.1 Bài toán gốc

```
┌──────────────────────────────────────────────────────┐
│  Bài toán: Làm thế nào để tách biệt việc          │
│  NHẬN KẾT NỐI (nhanh) khỏi XỬ LÝ REQUEST (chậm)? │
│                                                       │
│  Nếu cùng thread:                                   │
│    accept() → recv() → parse() → file I/O → send() │
│    Nếu file I/O chậm → accept() bị BLOATED      │
│    → Server từ chối kết nối mới                  │
│                                                       │
│  Nếu mỗi client = 1 thread:                       │
│    1000 clients = 1000 threads → OOM               │
│                                                       │
│  → Cần một CẦU NỐI có kiểm soát                    │
└──────────────────────────────────────────────────────┘
```

### 2.2 Producer-Consumer trong project

```
┌────────────────────────────────────────────────────────────────────┐
│  PRODUCER (Main Thread)                                         │
│                                                                  │
│  accept(listen_fd) → client_fd (→ TCP handshake done)         │
│         ↓                                                         │
│  socket_queue_enqueue(queue, client_fd)                         │
│         ↓                                                         │
│  • Chỉ làm việc NHẬN KẾT NỐI và ĐẨY VÀO QUEUE              │
│  • KHÔNG xử lý request                                        │
│  • Tốc độ: accept() nhanh hơn xử lý request → queue tích  │
│    luỹ nhưng có giới hạn                                     │
└────────────────────────────────────────────────────────────────────┘
                              ↓ enqueue
┌────────────────────────────────────────────────────────────────────┐
│  BOUNDED QUEUE (Cầu nối)                                        │
│                                                                  │
│  Chứa: client_fd (file descriptors)                           │
│  Đặc điểm: Fixed size (capacity)                             │
│  Cơ chế: FIFO — ai vào trước ra trước                       │
└────────────────────────────────────────────────────────────────────┘
                              ↑ dequeue
┌────────────────────────────────────────────────────────────────────┐
│  CONSUMERS (N Worker Threads)                                    │
│                                                                  │
│  socket_queue_dequeue() → client_fd                           │
│         ↓                                                         │
│  recv() → http_parse_request() → file_stat_path()            │
│         ↓                                                         │
│  send_file_response() → access_log_write()                    │
│         ↓                                                         │
│  close(client_fd)                                              │
│         ↓                                                         │
│  Lặp lại: dequeue() → xử lý request tiếp theo              │
└────────────────────────────────────────────────────────────────────┘
```

### 2.3 Code: Producer side

```c
// src/server.c:560–590
int server_run(server_t *server) {
    worker_context_t context = { .server = server };

    // Producer tạo consumers TRƯỚC khi bắt đầu nhận kết nối
    if (thread_pool_start(&server->pool, server->queue,
                         server->config.thread_count,
                         handle_client, &context) != 0) {
        return 1;
    }

    // === PRODUCER LOOP ===
    while (!server->should_stop) {
        // 1. Nhận kết nối mới (PRODUCE work)
        int client_fd = accept(server->listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (server->should_stop) break;
            continue;
        }

        // 2. Đẩy vào queue (PRODUCE to queue)
        queue_result_t enqueue_result =
            socket_queue_enqueue(server->queue, client_fd);

        if (enqueue_result == QUEUE_FULL) {
            // → BACKPRESSURE (xem phần 4)
            send_simple_response(client_fd, NULL, 503,
                             "Service Unavailable\n", 0, &response);
            close(client_fd);
        }
        // Nếu enqueue thành công → worker sẽ dequeue và xử lý
    }

    thread_pool_stop(&server->pool);  // Đợi consumers xong
    return 0;
}
```

### 2.4 Code: Consumer side

```c
// src/thread_pool.c:22–34
static void *worker_main(void *arg) {
    worker_args_t *worker_args = arg;
    thread_pool_t *pool = worker_args->pool;

    free(worker_args);  // args từ pthread_create()

    // === CONSUMER LOOP ===
    while (socket_queue_dequeue(pool->queue, &client_fd) == QUEUE_OK) {
        // Dequeue lấy client_fd từ queue
        // Xử lý request (CONSUMER: nhận work và hoàn thành nó)
        pool->handler(client_fd, pool->handler_context);
        // handle_client() gọi close(client_fd) bên trong
    }

    return NULL;  // Shutdown: dequeue trả QUEUE_CLOSED → thoát
}
```

### 2.5 Phân tích: Tại sao tách biệt này quan trọng?

```
Nếu KHÔNG TÁCH:
  accept() → recv() → parse() → [slow disk I/O] → send() → close()
  Mỗi request chiếm thread CẢ THỜI GIAN chờ đĩa
  → Qua tải: accept() backlog đầy → kernel từ chối SYN

Với Producer-Consumer:
  Main thread: accept() → enqueue()     [NHANH, < 1ms]
  Worker thread: dequeue() → I/O → send() [CHẬM, có thể 100ms]

  Kết quả:
  → Main thread luôn free nhận kết nối mới
  → Workers xử lý song song (4 workers cùng đọc 4 file)
  → Nếu workers chậm → queue tích lũy → backpressure kick in
```

---

## 3. Bounded Queue — Giải pháp hữu hạn

### 3.1 Unbounded vs Bounded

```
UNBOUNDED QUEUE (Không giới hạn):
  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───╲
  │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │10 │...
  └───┴───┴───┴───┴───┴───┴───┴───┴───┴────
  Producer: enqueue() enqueue() enqueue()
  Consumer:          dequeue()  dequeue()

  Vấn đề: Nếu producer nhanh hơn consumer VÔ HẠN:
  → Queue tích lũy 1 triệu items
  → Memory OOM → Server CRASH


BOUNDED QUEUE (Giới hạn):
  ┌───┬───┬───┬───┐
  │ 1 │ 2 │ 3 │ 4 │  capacity = 4
  └───┴───┴───┴───┘
  Producer: enqueue() enqueue() enqueue()
  Consumer:          dequeue()

  Nếu đầy:
  → enqueue() trả QUEUE_FULL NGAY
  → Producer quyết định: reject (503) hoặc block
  → KHÔNG bao giờ OOM
```

### 3.2 Circular Buffer Implementation

```c
// src/thread_pool.c:6–16
struct socket_queue {
    pthread_mutex_t mutex;       // Bảo vệ shared state
    pthread_cond_t  not_empty; // Workers chờ khi queue rỗng
    pthread_cond_t  not_full;  // Producer chờ khi queue đầy (dự phòng)
    int            *items;       // Mảng fd trên HEAP
    size_t          capacity;  // Kích thước tối đa
    size_t          head;       // Index để dequeue (FIFO)
    size_t          tail;       // Index để enqueue
    size_t          count;      // Số items hiện tại
    int             shutdown;    // Cờ shutdown
};
```

### 3.3 Circular Buffer Animation

```
capacity = 4, count = 0, head = 0, tail = 0

Sau enqueue(7), enqueue(8), enqueue(9):
┌───┬───┬───┬───┐
│ 7 │ 8 │ 9 │   │
└───┴───┴───┴───┘
↑           ↑
head        tail
count = 3

Sau enqueue(10) → count = 4 = capacity (ĐẦY):
┌───┬───┬───┬───┐
│ 7 │ 8 │ 9 │10 │
└───┴───┴───┴───┘
↑               ↑
head            tail(wrapped)
count = 4

Sau dequeue() → lấy 7:
┌───┬───┬───┬───┐
│   │ 8 │ 9 │10 │
└───┴───┴───┴───┘
    ↑       ↑
   head    tail
  (đã wrap)
count = 3

Sau enqueue(11):
┌───┬───┬───┬───┐
│11 │ 8 │ 9 │10 │  tail = (3+1)%4 = 0 → wrap!
└───┴───┴───┴───┘
↑           ↑
head        tail
```

### 3.4 Queue Full Check

```c
// src/thread_pool.c:90–111
queue_result_t socket_queue_enqueue(socket_queue_t *queue, int client_fd) {
    queue_result_t result = QUEUE_OK;

    pthread_mutex_lock(&queue->mutex);

    if (queue->shutdown) {
        result = QUEUE_CLOSED;
    } else if (queue->count == queue->capacity) {
        // → Trả QUEUE_FULL ngay, không block
        result = QUEUE_FULL;
    } else {
        queue->items[queue->tail] = client_fd;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);  // Wake 1 worker
        result = QUEUE_OK;
    }

    pthread_mutex_unlock(&queue->mutex);
    return result;
}
```

### 3.5 Thiết kế: Non-blocking Enqueue

```c
// Thiết kế quan trọng: Enqueue là NON-BLOCKING

// Option A: Blocking enqueue (KHÔNG dùng trong project)
while (queue->count == queue->capacity) {
    pthread_cond_wait(&queue->not_full, &queue->mutex);
}
// → Vấn đề: Main thread (producer) BLOCK trong khi đang nhận kết nối
// → Accept loop bị ngưng → không nhận connections mới
// → Kills throughput

// Option B: Non-blocking enqueue (DÙNG trong project)
if (queue->count == queue->capacity) {
    return QUEUE_FULL;  // Trả ngay lập tức
}
// → Accept loop tiếp tục chạy
// → Producer quyết định: reject connection
```

**Tại sao chọn non-blocking?** Main thread cần luôn free để accept connections mới. Nếu nó block trên queue full, server sẽ không nhận connections mới → tất cả clients bị từ chối dù queue sắp empty.

---

## 4. Backpressure — Từ chối có kiểm soát

### 4.1 Khái niệm Backpressure

```
┌──────────────────────────────────────────────────────────┐
│  Backpressure = Cơ chế upstream biết downstream bị quá tải │
│                  và phản ứng phù hợp                        │
│                                                            │
│  Producer quá nhanh → Consumer không kịp xử lý             │
│  → Queue tích lũy                                      │
│  → Queue đầy                                          │
│  → Backpressure: producer từ chối work                  │
│                                                            │
│  Trong HTTP context:                                      │
│  → Workers đang xử lý requests chậm                     │
│  → Queue đầy                                           │
│  → Trả HTTP 503 Service Unavailable                     │
│  → Client biết retry sau                                │
└──────────────────────────────────────────────────────────┘
```

### 4.2 Backpressure trong code

```c
// src/server.c:588–603
queue_result_t enqueue_result =
    socket_queue_enqueue(server->queue, client_fd);

if (enqueue_result == QUEUE_FULL) {
    // === BACKPRESSURE KICK IN ===
    response_result_t response = {0, 0};
    char client_ip[128];
    get_client_ip(client_fd, client_ip, sizeof(client_ip));

    // Gửi HTTP 503 Service Unavailable
    send_simple_response(client_fd, NULL, 503,
                       "Service Unavailable\n", 0, &response);

    // Log yêu cầu bị từ chối
    access_log_write(server->access_log, client_ip,
                    "-", 503, 0);

    // CRITICAL: Đóng socket để giải phóng fd
    close(client_fd);

    // Điểm hay: Server vẫn tiếp tục accept() connections mới
    // Không bị crash, không bị block
}
```

### 4.3 HTTP 503 — Service Unavailable

```
HTTP/1.1 503 Service Unavailable
Content-Type: text/plain
Content-Length: 21
Connection: close

Service Unavailable
```

| Khía cạnh | Chi tiết |
|-----------|---------|
| **Khi nào xảy ra?** | Queue đầy (workers đang bận xử lý requests) |
| **Trạng thái** | 503 — server tạm thời quá tải, biết sẽ phục hồi |
| **Connection header** | `close` — client đóng connection |
| **Tại sao không 429?** | 429 Too Many Requests là của API rate limiting |
| **Tại sao không 500?** | Đây không phải lỗi server — là load management |

### 4.4 Backpressure chain

```
Client → TCP SYN → Kernel accept backlog → accept()
                                        ↓
                              Queue [FULL] → 503 + close()
                                        ↓
Client nhận 503 → biết server quá tải
  → Retry sau 1-5s
  → Exponential backoff
  → Load giảm dần
  → Server phục hồi
```

### 4.5 Backpressure vs Load Shedding

```
BACKPRESSURE (Dùng trong project):
  → Khi queue đầy → từ chối connections mới
  → Server vẫn hoàn thành requests hiện tại
  → Hệ thống dần phục hồi khi load giảm

LOAD SHEDDING:
  → Từ chối % requests ngẫu nhiên khi quá tải
  → Ví dụ: nginx random(đến 100) > threshold → trả 503
  → Dùng khi không thể count queue (stateless)
```

---

## 5. Thread Pool — Tái sử dụng luồng

### 5.1 Thread-Per-Connection vs Thread Pool

```
THREAD-PER-CONNECTION:
  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
  │  Client 1   │     │  Client 2   │     │  Client N   │
  └──────┬───────┘     └──────┬───────┘     └──────┬───────┘
         ↓                      ↓                      ↓
  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
  │  Thread 1   │     │  Thread 2   │     │  Thread N   │
  │ (recv/parse/│     │ (recv/parse/│     │ (recv/parse/│
  │  send)      │     │  send)      │     │  send)      │
  └──────────────┘     └──────────────┘     └──────────────┘
         ↓                      ↓                      ↓
  Stack ~8MB × N        Stack ~8MB × N        Stack ~8MB × N

  Vấn đề:
  → N = 10000 clients → 80GB RAM chỉ cho stacks!
  → pthread_create() mỗi connection = overhead đáng kể
  → Context switching: 10000 threads = THREAD THRASHING

FIXED THREAD POOL:
  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
  │  Client 1   │     │  Client 2   │     │  Client N   │
  └──────┬───────┘     └──────┬───────┘     └──────┬───────┘
         ↓                      ↓                      ↓
  ┌──────────────────────────────────────────────────┐
  │              BOUNDED QUEUE                         │
  └──────────────────────────────────────────────────┘
                        ↓
         ┌──────────────┬──────────────┬──────────────┐
         │  Worker 1   │  Worker 2   │  Worker N   │
         │  dequeue()  │  dequeue()  │  dequeue()  │
         │  handle()   │  handle()   │  handle()   │
         └──────────────┴──────────────┴──────────────┘
         Stack ~8MB × 4     Stack ~8MB × 4     Stack ~8MB × 4

  Giải pháp:
  → N = 4 workers cố định (từ config -t 4)
  → 10000 clients chia sẻ 4 workers
  → 4 × 8MB = 32MB stack (thay vì 80GB)
  → Thread reuse: tạo 1 lần, dùng mãi mãi
```

### 5.2 Thread Pool trong code

```c
// src/thread_pool.c:165–201
int thread_pool_start(thread_pool_t *pool,
                     socket_queue_t *queue,
                     int thread_count,
                     thread_pool_handler_t handler,
                     void *context) {

    if (pool == NULL || queue == NULL || thread_count <= 0 || handler == NULL) {
        return -1;
    }

    // Cấp phát mảng lưu thread IDs
    pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));
    if (threads == NULL) {
        return -1;
    }

    // Lưu metadata
    pool->queue = queue;
    pool->threads = threads;
    pool->thread_count = thread_count;
    pool->handler = handler;           // = handle_client()
    pool->handler_context = context;   // = worker_context

    // === TẠO N THREADS MỘT LẦN, KHÔNG PHẢI MỖI KẾT NỐI ===
    for (int i = 0; i < thread_count; i++) {
        worker_args_t *args = malloc(sizeof(*args));
        if (args == NULL) {
            pool->thread_count = i;
            thread_pool_stop(pool);  // Cleanup những threads đã tạo
            return -1;
        }
        args->pool = pool;

        if (pthread_create(&threads[i], NULL, worker_main, args) != 0) {
            free(args);
            pool->thread_count = i;
            thread_pool_stop(pool);
            return -1;
        }
    }

    return 0;
}
```

### 5.3 Configurable thread count

```
Trong project, số workers được config từ CLI:

./httpd -t 4 -q 64 ...

server_config_t config = {
    .thread_count = 4,     // Số workers (configurable)
    .queue_capacity = 64,  // Queue size (configurable)
    ...
};
```

| Config | Giá trị mặc định | Tác động |
|--------|----------------|--------|
| `-t threads` | 4 | Số worker threads xử lý song song |
| `-q queue` | 64 | Số connections có thể chờ trong queue |

**Tuning:**
- Threads > CPU cores → context switching overhead
- Threads < CPU cores → CPU idle khi I/O
- Tối ưu: N workers ≈ N CPU cores (cho CPU-bound workload)
- N workers > CPU cores (cho I/O-bound workload)

### 5.4 Work Stealing vs Fixed Assignment

```
FIXED ASSIGNMENT (Dùng trong project):
  Client 1 → Worker 1 (cứng nhắc)
  Client 2 → Worker 2 (cứng nhắc)
  → Worker 1 bận đọc file lớn, Worker 2 rảnh rỗi
  → Load không đều

WORK STEALING (nginx, Go runtime):
  Worker 1: nhận 5 requests
  Worker 2: nhận 0 requests
  → Worker 2 "steal" task từ Worker 1
  → Load cân bằng tự động

Tại sao project dùng Fixed:
  → Đơn giản hơn nhiều
  → Đủ tốt cho quy mô project
  → nginx dùng work stealing vì scale lớn
```

---

## 6. Spurious Wakeups — Wake lên mà không có lý do

### 6.1 Định nghĩa chính thức

**Spurious wakeup** = một thread bị đánh thức khỏi `pthread_cond_wait()` **mà không có** bất kỳ lời gọi `pthread_cond_signal()` hoặc `pthread_cond_broadcast()` nào.

```
Nguồn gốc:
  • POSIX specification cho phép (không bắt buộc) spurious wakeups
  • Kernel implementation có thể wake thread để:
    - Tránh deadlock trong edge cases với signal handlers
    - Tối ưu hóa trên multi-core systems
    - Hệ thống con kernel wake thread để resource availability thay đổi

Không có pattern SAI nào gây ra spurious wakeups — đây là spec behavior
```

### 6.2 Minh họa timeline

```
T=0: Queue EMPTY, Worker A gọi dequeue()

T=1: Worker A: mutex_lock()
     Worker A: while (queue->count == 0 && !shutdown)
     Worker A: pthread_cond_wait(&not_empty, &mutex)
     Worker A: [atomic: unlock(mutex) + sleep]

T=2: (Không ai gọi signal)

T=3: ⚠️ SPURIOUS WAKEUP
     Kernel đánh thức Worker A
     Worker A: [atomic: relock(mutex)]
     Worker A: quay lên kiểm tra condition

T=4: Worker A: while (queue->count == 0 && !shutdown) → TRUE
     Worker A: → ngủ lại ngay
     ✅ KHÔNG CÓ LỖI
```

### 6.3 Code: Vòng lặp đúng

```c
// src/thread_pool.c:113–135
queue_result_t socket_queue_dequeue(socket_queue_t *queue, int *client_fd) {
    if (queue == NULL || client_fd == NULL) {
        return QUEUE_ERROR;
    }

    pthread_mutex_lock(&queue->mutex);

    // === BẮT BUỘC DÙNG while, KHÔNG PHẢI if ===
    while (queue->count == 0 && !queue->shutdown) {
        // Nếu wake vì SPURIOUS hoặc chưa có signal thật:
        // → Vòng lên kiểm tra lại condition
        // → Vẫn còn count == 0 → ngủ tiếp
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    // Đến đây: count > 0 HOẶC shutdown == 1

    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return QUEUE_CLOSED;  // Shutdown path
    }

    // === LẤY ITEM ===
    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);
    return QUEUE_OK;
}
```

### 6.4 Spurious Wakeup vs Lost Wakeup

```
SPURIOUS WAKEUP:
  Thread wake LÊN mà KHÔNG CÓ signal
  → while() giải quyết: kiểm tra lại, ngủ tiếp
  → KHÔNG gây lỗi logic

LOST WAKEUP (Race condition thật sự):
  Producer: queue->count++  → NHƯNG chưa signal
  Worker: wake lên → count vẫn là 0
  → KHÔNG có item để dequeue

  Trong project:
  → count++ VÀ signal ĐƯỢC BAO TRONG CÙNG MUTEX
  → atomic: không có window race
  → Lost wakeup KHÔNG XẢY RA

Vấn đề với code SAI:
pthread_mutex_lock(&queue->mutex);
queue->count++;
// ⚠️ INTERRUPT ở đây → signal không được gọi
pthread_cond_signal(&queue->not_empty);
pthread_mutex_unlock(&queue->mutex);
```

### 6.5 SAI pattern

```c
// ❌ SAI: Dùng if
while (1) {
    if (queue->count == 0) {
        pthread_cond_wait(&not_empty, &mutex);
    }
    if (queue->count > 0) {
        // Lấy item → CÓ THỂ LẤY TRASH DATA nếu spurious
        dequeue();
    }
}

// ❌ SAI: Signal không trong mutex
pthread_mutex_unlock(&mutex);
queue->count++;
pthread_cond_signal(&not_empty);  // RACE: worker có thể miss signal
pthread_mutex_lock(&mutex);
```

---

## 7. Tích hợp toàn bộ hệ thống

### 7.1 Full system diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │              MAIN THREAD (Producer)                           │   │
│  │                                                               │   │
│  │  socket() → bind() → listen() → SOMAXCONN                │   │
│  │       ↓                                                     │   │
│  │  while (!should_stop) {                                    │   │
│  │    client_fd = accept(listen_fd)                           │   │
│  │    result = enqueue(client_fd)                            │   │
│  │    if (result == QUEUE_FULL) → 503 + close()            │   │
│  │  }                                                         │   │
│  │       ↓                                                     │   │
│  │  thread_pool_stop() → join all workers                   │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                              │ enqueue                              │
│                              ↓                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  BOUNDED QUEUE (capacity=64)                              │   │
│  │                                                               │   │
│  │  State tracking: head, tail, count, capacity               │   │
│  │  Sync: mutex, not_empty, not_full                        │   │
│  │  Shutdown: broadcast(not_empty) + broadcast(not_full)      │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                              │ dequeue                              │
│                 ┌────────────┴────────────┐                       │
│                 ↓                         ↓                        │
│  ┌──────────────────────┐  ┌──────────────────────┐             │
│  │   Worker Thread 1     │  │   Worker Thread 2     │             │
│  │  handle_client(fd)     │  │  handle_client(fd)     │             │
│  │  recv() → parse()    │  │  recv() → parse()    │             │
│  │  file_stat_path()     │  │  file_stat_path()     │             │
│  │  send_file_response() │  │  send_file_response() │             │
│  │  access_log_write()  │  │  access_log_write()  │             │
│  │  close(fd)           │  │  close(fd)           │             │
│  └──────────────────────┘  └──────────────────────┘             │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.2 Stress test: 120 concurrent clients

```
bench/bench.sh 120 clients →

Client 1 ──┐
Client 2 ──┤
Client 3 ──┤
  ...    ──┼──→ accept() → enqueue() ──→ Queue ──→ Workers
Client 120 ─┘

Thread Pool (4 workers):
  Worker 1: dequeue() → recv() → parse() → file → send() → close()
  Worker 2: dequeue() → recv() → parse() → file → send() → close()
  Worker 3: dequeue() → recv() → parse() → file → send() → close()
  Worker 4: dequeue() → recv() → parse() → file → send() → close()

  Requests 1-4: đang xử lý
  Requests 5-64: trong queue (chờ)
  Requests 65-120: đến sau, queue ĐẦY → 503

  Nhưng benchmark expect 0 failures?
  → 120 requests đến gần như đồng thời
  → Queue = 64, Workers = 4
  → Max in-flight = 4 + 64 = 68 requests
  → 120 - 68 = 52 bị 503?

  → Thực tế: requests rất nhanh (localhost ~0ms latency)
  → Workers xử lý xong trước khi queue đầy
  → Throughput ~114 req/s >> arrival rate
```

### 7.3 Stress test: Queue Full scenario

```
Giả sử: workers đang xử lý requests chậm (đĩa quá chậm)

T=0: Workers đang busy (4 workers)
T=1: Queue đầy (64 items)
T=2: Client 65 kết nối → accept()
T=3: enqueue() → QUEUE_FULL
T=4: 503 + close()
T=5: Client 65 nhận 503 → retry sau 1s

Meanwhile:
T=6: Worker 1 xong → dequeue()
T=7: Worker 2 xong → dequeue()
T=8: Worker 3 xong → dequeue()
T=9: Worker 4 xong → dequeue()
T=10: Queue rỗng
T=11: Client 66-68 accept() → enqueue() → dequeue() → xử lý

→ Backpressure tự động điều chỉnh load
→ Client retries giúp distribute load theo thời gian
→ Server KHÔNG crash
```

---

## 8. Phân tích sâu: Tại sao thiết kế này đúng

### 8.1 Tính đúng đắn (Correctness)

| Invariant | Proof |
|-----------|--------|
| `count` không bao giờ âm | Chỉ `enqueue()` tăng, chỉ `dequeue()` giảm, trong mutex |
| `count` không bao giờ > `capacity` | Enqueue kiểm tra `count == capacity` → trả `QUEUE_FULL` |
| Head luôn hợp lệ | `head = (head + 1) % capacity`, khởi tạo 0 |
| Tail luôn hợp lệ | `tail = (tail + 1) % capacity`, khởi tạo 0 |
| FIFO ordering | Dequeue luôn lấy từ `head`, enqueue luôn đặt tại `tail` |
| No lost wakeups | `count++` và `signal()` trong cùng mutex |
| No spurious data | `while` loop kiểm tra lại sau mỗi wake |

### 8.2 Tính không deadlock

```
Deadlock = 4 điều kiện (Coffman):

1. Mutual Exclusion:
   ✓ Queue mutex chỉ cho 1 thread tại 1 thời điểm

2. Hold and Wait:
   ✗ Thread KHÔNG giữ mutex trong khi chờ
   Enqueue: lock → thao tác → unlock (rất nhanh, ~microseconds)
   Dequeue: lock → thao tác → unlock (rất nhanh, ~microseconds)

3. No Preemption:
   ✓ Mutex không bị preempt bởi thread khác

4. Circular Wait:
   ✗ Không có chuỗi A→B→C→A
   Main: chỉ enqueue, không chờ gì
   Worker: chỉ dequeue, không chờ gì

→ DEADLOCK KHÔNG THỂ XẢY RA
```

### 8.3 Tính không starvation

```
Starvation = Thread không bao giờ nhận được resource

Trong project:

PRODUCER STARVATION?
  → Producer chỉ bị block khi `accept()` kernel backlog đầy
  → OS handle backlog, không phải code này
  → Không starvation

CONSUMER STARVATION?
  → N workers cùng đợi `not_empty`
  → Signal wake 1 trong số họ
  → Nếu workload đều: mỗi worker nhận ~equal share
  → FIFO queue: ai vào trước ra trước
  → Không starvation

QUEUE FULL STARVATION?
  → Enqueue non-blocking → trả QUEUE_FULL → client retry
  → Producer không bị block → không starvation
```

### 8.4 So sánh với alternative designs

```
ALTERNATIVE 1: Thread-per-connection
  Pros: Simple, natural mapping
  Cons: OOM với 10K connections, pthread_create() overhead

ALTERNATIVE 2: Event-driven (epoll/kqueue)
  Pros: 1 thread, scale to 100K idle connections
  Cons: Complex state machines, callback hell, harder to debug

ALTERNATIVE 3: Asynchronous I/O (io_uring)
  Pros: Zero-copy, very high throughput
  Cons: Linux 5.1+, complex API, bleeding edge

PROJECT CHOICE: Thread Pool + Bounded Queue
  Pros:
    - Đơn giản để implement và debug
    - Đủ tốt cho quy mô: 100-1000 concurrent clients
    - Blocking I/O: sequential code, dễ đọc
    - Pre-create threads: zero pthread_create() overhead per request
    - Bounded queue: backpressure, no OOM
  Cons:
    - Context switch overhead khi threads block
    - 1 thread per active connection (idle keep-alive chiếm thread)
    - Không scale tốt với 10K+ idle connections
  → PHÙ HỢP VỚI PROJECT SCOPE
```

---

## Tổng kết Level 4 — Quick Reference

```
┌─────────────────────────────────────────────────────────────┐
│ PRODUCER-CONSUMER                                           │
│                                                             │
│  Producer (main):  accept() → enqueue()                  │
│  Consumers (N workers): dequeue() → handle()             │
│  Queue = cầu nối giữa 2 tốc độ khác nhau                │
│  → Producer nhanh, Consumers chậm → queue tích lũy       │
│  → Bounded queue giới hạn memory                         │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ BOUNDED QUEUE (Circular Buffer)                           │
│                                                             │
│  head = (head + 1) % capacity  ← dequeue                 │
│  tail = (tail + 1) % capacity  ← enqueue                 │
│  count tracker để phân biệt empty vs full (head==tail)  │
│  Non-blocking enqueue: đầy → QUEUE_FULL ngay             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ BACKPRESSURE                                               │
│                                                             │
│  Queue đầy → enqueue() → QUEUE_FULL                       │
│  → Main thread gửi HTTP 503 + close()                   │
│  → Client retry sau → load tự phân bố theo thời gian   │
│  → KHÔNG OOM, KHÔNG crash                              │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ THREAD POOL                                                │
│                                                             │
│  Pre-create N threads lúc khởi động                     │
│  Tái sử dụng threads cho tất cả requests               │
│  vs Thread-per-connection: tiết kiệm RAM + pthread_create │
│  Configurable: -t N workers, -q M queue capacity        │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ SPURIOUS WAKEUPS                                          │
│                                                             │
│  pthread_cond_wait() CÓ THỂ wake mà không có signal       │
│  → Dùng while(cond) KHÔNG if(cond)                      │
│  → Wake lên → kiểm tra lại → vẫn sai → ngủ tiếp     │
│  → KHÔNG gây lỗi logic                                   │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ FULL SYSTEM INVARIANTS                                     │
│                                                             │
│  No deadlock: No hold-and-wait, no circular wait          │
│  No starvation: FIFO queue, non-blocking producer       │
│  No data races: mutex bảo vệ tất cả queue operations   │
│  No lost wakeups: count++ và signal() trong cùng mutex │
│  Correctness: count ∈ [0, capacity]                   │
└─────────────────────────────────────────────────────────────┘
```
