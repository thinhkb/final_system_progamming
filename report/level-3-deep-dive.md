# Level 3 Deep Dive — Threads (pthreads)

Level 3 là tầng khó nhất trong System Programming. Tại đây, một tiến trình (process) đơn luồng mở rộng thành nhiều luồng (threads) chạy song song, chia sẻ tài nguyên, và cần đồng bộ hóa để tránh race conditions. Đây là trái tim của concurrency trong project này.

---

## Mục lục

1. [Thread vs Process — Memory Architecture](#1-thread-vs-process--memory-architecture)
2. [Thread Lifecycle — `pthread_create()` và `pthread_join()`](#2-thread-lifecycle--pthread_create-và-pthread_join)
3. [Producer-Consumer Pattern — Tổng quan](#3-producer-consumer-pattern--tổng-quan)
4. [Bounded Queue — Circular Buffer](#4-bounded-queue--circular-buffer)
5. [Mutex — Mutual Exclusion](#5-mutex--mutual-exclusion)
6. [Condition Variables — Efficient Sleeping](#6-condition-variables--efficient-sleeping)
7. [Spurious Wakeups — Tại sao dùng `while` thay vì `if`](#7-spurious-wakeups--tại-sao-dùng-while-thay-vì-if)
8. [Signal vs Broadcast](#8-signal-vs-broadcast)
9. [Graceful Shutdown — Tắt máy an toàn](#9-graceful-shutdown--tắt-máy-an-toàn)
10. [Deadlock Analysis](#10-deadlock-analysis)
11. [Code Walkthrough: Producer Side](#11-code-walkthrough-producer-side)
12. [Code Walkthrough: Consumer Side](#12-code-walkthrough-consumer-side)

---

## 1. Thread vs Process — Memory Architecture

### 1.1 Hai mô hình song song

```
Process Model (fork):
┌─────────────┐     ┌─────────────┐
│  Parent    │     │   Child     │
│  Process   │     │   Process   │
├─────────────┤     ├─────────────┤
│  Code      │     │  (copied)  │
│  (shared) │     │  Code      │
├─────────────┤     ├─────────────┤
│  Heap      │     │  (copied)  │
│  (isolated)│     │  Heap      │
├─────────────┤     ├─────────────┤
│  Stack     │     │  (copied)  │
│  (isolated)│     │  Stack     │
├─────────────┤     ├─────────────┤
│  FD Table  │     │  (copied)  │
│  (isolated)│     │  FD Table  │
└─────────────┘     └─────────────┘
    ↓ fork() = copy toán bộ process image
    Cost: ĐẮT (copy toàn bộ address space)

Thread Model (pthread):
┌─────────────────────────────────────────────┐
│              Single Process                    │
├─────────────────────────────────────────────┤
│  Code (shared — read-only)                   │
├─────────────────────────────────────────────┤
│  Heap (shared — all threads access)          │
├───────────┬───────────┬─────────────────────┤
│  Stack 1  │  Stack 2  │  Stack N            │
│  (Thread 1)│  (Thread 2)│  (Worker N)     │
├───────────┴───────────┴─────────────────────┤
│  FD Table (shared — all threads access)     │
│  Global vars (shared — all threads access)   │
└─────────────────────────────────────────────┘
    ↓ pthread_create() = thêm execution context
    Cost: RẺ (chỉ tạo stack + thread metadata)
```

### 1.2 Memory Sharing trong project

```c
// src/server.c — Shared by all threads:
typedef struct server {
    server_config_t config;        // Shared: all threads read config
    int listen_fd;               // Shared: main thread accepts, workers don't use
    volatile sig_atomic_t should_stop; // Shared: signal handler writes, main reads
    socket_queue_t *queue;        // Shared: producer enqueues, consumers dequeue
    thread_pool_t pool;           // Shared: worker metadata
    access_log_t *access_log;    // Shared: all workers write to log
} server_t;
```

Mọi worker thread đều truy cập:
- `server->queue` (hàng đợi socket)
- `server->access_log` (file log)

### 1.3 Per-thread data

```c
// src/server.c:400–411 — Mỗi worker có stack riêng:
void handle_client(int client_fd, void *context) {
    // Tất cả biến này nằm trên STACK của thread đó
    char buffer[READ_BUFFER_SIZE];      // 8KB stack — PRIVATE
    char request_line[256];            // PRIVATE
    char client_ip[128];               // PRIVATE
    http_request_t request;           // PRIVATE
    file_info_t info;                // PRIVATE
    // ...
}
```

### 1.4 Thread ID và Stack Size

```c
// src/thread_pool.c:167–181
pthread_t *threads;
threads = calloc((size_t)thread_count, sizeof(*threads));

for (int i = 0; i < thread_count; i++) {
    pthread_create(&threads[i], NULL, worker_main, args);
}
```

| Resource | Default | Có thể thay đổi |
|-----------|---------|-----------------|
| Stack size per thread | 8MB (Linux/macOS) | `pthread_attr_setstacksize()` |
| Max threads per process | ~1000 (ulimit) | Tăng với `ulimit -s` |
| Thread ID | opaque type | `pthread_self()` để lấy |

### 1.5 Tại sao không dùng `fork()`?

| Aspect | `fork()` (Process) | `pthread_create()` (Thread) |
|--------|--------------------|---------------------------|
| Memory sharing | None (COW) | Full (heap, code, FD, globals) |
| Creation cost | Heavy (copy entire address space) | Light (just stack + metadata) |
| IPC complexity | Pipes, sockets, shared memory | Direct pointer access |
| Context switch | Expensive (MMU switch) | Cheaper (same address space) |
| Crash isolation | Child crash ≠ parent crash | Thread crash = process crash |
| Use case in project | Not used | All workers |

Project này dùng threads vì workers cần chia sẻ queue và log — threads cho phép truy cập pointer trực tiếp mà không cần IPC phức tạp.

---

## 2. Thread Lifecycle — `pthread_create()` và `pthread_join()`

### 2.1 `pthread_create()` — Tạo thread

```c
// src/thread_pool.c:167–181
pthread_t *threads;
threads = calloc((size_t)thread_count, sizeof(*threads));

for (int i = 0; i < thread_count; i++) {
    worker_args_t *args = malloc(sizeof(*args));
    args->pool = pool;

    if (pthread_create(&threads[i], NULL, worker_main, args) != 0) {
        pool->thread_count = i;
        thread_pool_stop(pool);
        return -1;
    }
}
```

**Thứ tự quan trọng:**
1. `calloc()` tạo mảng `pthread_t` — lưu thread IDs
2. Mỗi thread được tạo với `args` trên heap (sau đó worker tự `free()`)
3. **Nếu thread tạo thất bại** → `thread_pool_stop()` cleanup những threads đã tạo
4. **Nếu args malloc thất bại** → `free(args)`, stop pool

### 2.2 `pthread_join()` — Đợi thread kết thúc

```c
// src/thread_pool.c:193–202
void thread_pool_stop(thread_pool_t *pool) {
    pthread_t *threads = pool->threads;

    socket_queue_shutdown(pool->queue);  // Signal shutdown

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(threads[i], NULL);  // Đợi thread i kết thúc
    }

    free(threads);
    pool->threads = NULL;
}
```

**`pthread_join()` semantics:**
- Gọi thread **block** cho đến khi target thread exit
- Nếu target đã exit → return ngay lập tức
- Return value (`NULL` trong project) được discard
- Sau `join()`, thread ID có thể reuse

### 2.3 Worker main loop

```c
// src/thread_pool.c:22–34
static void *worker_main(void *arg) {
    worker_args_t *worker_args = arg;
    thread_pool_t *pool = worker_args->pool;

    free(worker_args);  // Giải phóng args mà pthread_create truyền vào

    // Vòng lặp vĩnh viễn:
    while (socket_queue_dequeue(pool->queue, &client_fd) == QUEUE_OK) {
        pool->handler(client_fd, pool->handler_context);  // → handle_client()
    }

    return NULL;  // Khi queue shutdown, thoát
}
```

---

## 3. Producer-Consumer Pattern — Tổng quan

### 3.1 Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│  MAIN THREAD (Producer)                                             │
│                                                                    │
│    accept(listen_fd) → client_fd                                │
│           ↓                                                         │
│    socket_queue_enqueue(queue, client_fd)                         │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
                              ↓ enqueue (mutex + signal)
┌────────────────────────────────────────────────────────────────────┐
│  BOUNDED SOCKET QUEUE (Circular Buffer)                          │
│                                                                    │
│  ┌───┬───┬───┬───┐                                               │
│  │ 7 │ 8 │ 9 │   │  head=0  tail=3  count=3  capacity=4       │
│  └───┴───┴───┴───┘                                               │
│  items[]                                                           │
└────────────────────────────────────────────────────────────────────┘
                              ↑ dequeue (mutex + wait)
┌────────────────────────────────────────────────────────────────────┐
│  WORKER THREADS (Consumers) × N                                   │
│                                                                    │
│    socket_queue_dequeue(queue, &client_fd)                       │
│           ↓                                                         │
│    handle_client(client_fd, context)                              │
│           ↓                                                         │
│    recv() → parse → file → send() → close()                      │
└────────────────────────────────────────────────────────────────────┘
```

### 3.2 Producer (Main Thread)

```c
// src/server.c:557–581
int server_run(server_t *server) {
    while (!server->should_stop) {
        int client_fd = accept(server->listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        queue_result_t result = socket_queue_enqueue(server->queue, client_fd);

        if (result == QUEUE_FULL) {
            // Backpressure: queue đầy → từ chối ngay
            send_simple_response(client_fd, NULL, 503, "Service Unavailable\n", 0, &response);
            close(client_fd);
        }
    }
}
```

### 3.3 Consumers (Worker Threads)

```c
// src/thread_pool.c:22–34
while (socket_queue_dequeue(pool->queue, &client_fd) == QUEUE_OK) {
    pool->handler(client_fd, pool->handler_context);  // Xử lý request
    // close(client_fd) được gọi bên trong handle_client()
}
```

---

## 4. Bounded Queue — Circular Buffer

### 4.1 Data Structure

```c
// src/thread_pool.c:6–16
struct socket_queue {
    pthread_mutex_t mutex;       // Bảo vệ tất cả shared state
    pthread_cond_t  not_empty;   // Workers chờ khi queue rỗng
    pthread_cond_t  not_full;    // Producer chờ khi queue đầy (cho future blocking enqueue)
    int            *items;        // Mảng socket fd trên heap
    size_t          capacity;    // Kích thước tối đa
    size_t          head;         // Index để dequeue (FIFO)
    size_t          tail;         // Index để enqueue
    size_t          count;        // Số items hiện tại
    int             shutdown;      // Cờ shutdown
};
```

### 4.2 Circular Buffer Animation

```
capacity = 4

Step 0: empty
        head=0  tail=0  count=0
┌───┬───┬───┬───┐
│   │   │   │   │
└───┴───┴───┴───┘

Step 1: enqueue(7)
        head=0  tail=1  count=1
┌───┬───┬───┬───┐
│ 7 │   │   │   │
└───┴───┴───┴───┘

Step 2: enqueue(8)
        head=0  tail=2  count=2
┌───┬───┬───┬───┐
│ 7 │ 8 │   │   │
└───┴───┴───┴───┘

Step 3: enqueue(9)
        head=0  tail=3  count=3
┌───┬───┬───┬───┐
│ 7 │ 8 │ 9 │   │
└───┴───┴───┴───┘

Step 4: enqueue(10) → capacity reached
        head=0  tail=0  count=4
┌───┬───┬───┬───┐
│ 7 │ 8 │ 9 │10 │
└───┴───┴───┴───┘

Step 5: dequeue() → lấy 7
        head=1  tail=0  count=3
┌───┬───┬───┬───┐
│   │ 8 │ 9 │10 │  ← head trỏ vào slot cũ (overwrite được)
└───┴───┴───┴───┘

Step 6: enqueue(11)
        head=1  tail=1  count=3  (tail wrapped!)
┌───┬───┬───┬───┐
│11 │ 8 │ 9 │10 │
└───┴───┴───┴───┘

Modulo arithmetic: tail = (tail + 1) % capacity
```

### 4.3 Tại sao modulo arithmetic đúng?

**Invariant 1:** `count == 0` khi `head == tail`
**Invariant 2:** `count == capacity` khi `head == tail` (after wrap)

**Để phân biệt 2 trạng thái này, project dùng `count` tracker riêng.**

Nếu không dùng `count`:
- head=tail khi empty
- head=tail khi full
- Không biết được là empty hay full!

Project dùng `count` để track chính xác số items.

### 4.4 Wrap-around edge cases

```c
// src/thread_pool.c:103–106 — Enqueue
queue->items[queue->tail] = client_fd;
queue->tail = (queue->tail + 1) % queue->capacity;  // ← MODULO
queue->count++;
pthread_cond_signal(&queue->not_empty);

// src/thread_pool.c:128–130 — Dequeue
*client_fd = queue->items[queue->head];
queue->head = (queue->head + 1) % queue->capacity;  // ← MODULO
queue->count--;
pthread_cond_signal(&queue->not_full);
```

**Tại sao dùng `%`?** Nếu tail=3, capacity=4, thì (3+1)%4 = 0 → quay về đầu mảng. Nếu không có `%`, tail sẽ thành 4 → out-of-bounds access → crash.

---

## 5. Mutex — Mutual Exclusion

### 5.1 Race Condition không có Mutex

```
❌ WITHOUT MUTEX (BROKEN):

Thread A: read count → 3
Thread B: read count → 3          ← Cả hai đọc cùng giá trị!
Thread A: count = 3 + 1 = 4
Thread B: count = 3 + 1 = 4      ← Kết quả SAI (phải là 5)
Thread A: write count = 4
Thread B: write count = 4          ← Ghi đè!

✅ WITH MUTEX (CORRECT):

Thread A: mutex_lock()
Thread A: read count → 3
Thread A: count = 3 + 1 = 4
Thread A: write count = 4
Thread A: mutex_unlock()

Thread B: mutex_lock()
Thread B: read count → 4          ← Thấy giá trị ĐÚNG
Thread B: count = 4 + 1 = 5
Thread B: write count = 5
Thread B: mutex_unlock()
```

### 5.2 Mutex trong project

```c
// src/thread_pool.c:36–76 — Initialization
socket_queue_t *created;
created->items = calloc(capacity, sizeof(int));

if (pthread_mutex_init(&created->mutex, NULL) != 0) {  // ← Khởi tạo
    free(created->items);
    free(created);
    return -1;
}

// src/thread_pool.c:97 — Lock everywhere
pthread_mutex_lock(&queue->mutex);
// ... thao tác queue ...
pthread_mutex_unlock(&queue->mutex);

// src/thread_pool.c:78–88 — Cleanup
pthread_mutex_destroy(&queue->mutex);  // ← Phải destroy khi xong
free(queue->items);
free(queue);
```

### 5.3 Mutex — Tại sao cả 2 operation phải trong critical section?

```c
// ❌ SAI: RACE CONDITION
// Enqueue: thao tác 3 bước phải atomic
pthread_mutex_lock(&queue->mutex);
items[tail] = client_fd;
tail = (tail + 1) % capacity;
count++;
pthread_cond_signal(&queue->not_empty);
pthread_mutex_unlock(&queue->mutex);

// ❌ SAI: Nếu chỉ lock 1 bước:
// Thread A: items[tail] = fd  → lock  → unlock
// Thread B: items[tail] = fd  → lock  → unlock
// Thread A: tail++           → lock  → unlock
// Thread B: tail++           → lock  → unlock
// → Kết quả: tail tăng 2 lần nhưng chỉ ghi 1 item
```

### 5.4 Mutex properties trong project

```c
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;  // Mặc định
// Hoặc:
pthread_mutex_init(&mutex, NULL);  // NULL = default attributes
```

| Attribute | Giá trị | Ý nghĩa |
|-----------|---------|---------|
| `PTHREAD_MUTEX_NORMAL` | Default | Deadlock nếu unlock không lock |
| `PTHREAD_MUTEX_RECURSIVE` | Không dùng | Cho phép cùng thread lock nhiều lần |
| Robust | Not set | Không hỗ trợ recovery khi owner crash |

---

## 6. Condition Variables — Efficient Sleeping

### 6.1 Vấn đề: Busy-Waiting

```
❌ BUSY-WAITING (Lãng phí CPU 100%):

while (queue->count == 0) {
    // Mỗi vòng lặp: kiểm tra count
    // CPU chạy 100% chỉ để kiểm tra một biến
    // Tốn điện, nóng máy, ảnh hưởng tasks khác
}

✅ CONDITION VARIABLE (Ngủ hiệu quả):

while (queue->count == 0) {
    pthread_cond_wait(&not_empty, &mutex);
    // Thread SLEEP → tiêu tốn 0% CPU
    // Kernel đánh thức khi có signal
}
```

### 6.2 `pthread_cond_wait()` — Atomic Unlock + Sleep

```c
// src/thread_pool.c:118–121
while (queue->count == 0 && !queue->shutdown) {
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
}
```

**Đây là operation PHỨC TẠP NHẤT trong pthreads. 4 bước atomic:**

```
pthread_cond_wait(&cond, &mutex) bên trong kernel:
┌──────────────────────────────────────────────────────────────┐
│  1. [Thread đang hold mutex]                               │
│                                                              │
│  2. [Kernel nhận lock]                                    │
│                                                              │
│  3. [Kernel unlock mutex] ← THREAD SLEEP                  │
│     (atomic: không có window race)                        │
│                                                              │
│  4. [Kernel đặt thread vào wait queue của condvar]       │
│                                                              │
│  ... (Thread sleeping, 0% CPU) ...                         │
│                                                              │
│  5. [Signal/Broadcast được gọi]                            │
│                                                              │
│  6. [Kernel wake thread]                                   │
│                                                              │
│  7. [Kernel relock mutex] ← THREAD WOKEN                  │
│                                                              │
│  8. [Return cho user space]                                │
└──────────────────────────────────────────────────────────────┘
```

### 6.3 Hai condition variables trong project

```c
struct socket_queue {
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;   // Workers chờ khi count == 0
    pthread_cond_t  not_full;    // (Dự phòng cho blocking enqueue)
};
```

**Mỗi CV có wait queue riêng:**
- `not_empty`: tất cả idle workers đợi ở đây
- `not_full`: producer sẽ đợi ở đây (nếu dùng blocking enqueue)

### 6.4 Signal vs Wait semantics

```c
// ENQUEUE (Producer) — gửi signal khi có item mới:
pthread_mutex_lock(&queue->mutex);
queue->items[queue->tail] = client_fd;
queue->tail = (queue->tail + 1) % queue->capacity;
queue->count++;
pthread_cond_signal(&queue->not_empty);  // ← Đánh thức 1 worker
pthread_mutex_unlock(&queue->mutex);

// DEQUEUE (Consumer) — đợi khi rỗng:
pthread_mutex_lock(&queue->mutex);
while (queue->count == 0 && !queue->shutdown) {
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
}
if (queue->count > 0) {
    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);  // ← Đánh thức producer (dự phòng)
}
pthread_mutex_unlock(&queue->mutex);
```

---

## 7. Spurious Wakeups — Tại sao dùng `while` thay vì `if`

### 7.1 Định nghĩa

**Spurious wakeup** là hiện tượng POSIX cho phép kernel đánh thức một thread từ `pthread_cond_wait()` mà **không có** bất kỳ `signal()` hay `broadcast()` nào được gọi.

### 7.2 Tại sao kernel làm vậy?

```
Lý do kỹ thuật:
- Để tránh DEADLOCK trong một số edge cases với signal handlers
- Để tối ưu hóa scheduling trên multi-core systems
- Để tránh "lost wakeup" race conditions

Đây là spec CỦA POSIX — implementation phải cho phép spurious wakeups
```

### 7.3 Minh họa với `if` (SAI)

```
Timeline:

T=0: queue->count = 0
     Worker A: mutex_lock()
     Worker A: if (queue->count == 0) → TRUE → vào pthread_cond_wait
     Worker A: [Kernel: atomic unlock + sleep]
     Worker A: (mutex đã unlock, Worker A đang ngủ)
     
T=1: Producer: mutex_lock()
     Producer: count = 1
     Producer: pthread_cond_signal(&not_empty)
     Worker A: [Kernel: wake up, relock mutex]  ← CÓ THỂ LÀ SPURIOUS
     Worker A: (vẫn trong if branch, không re-check)

T=2: ⚠️ NHƯNG KHOAN — có thể trước khi Worker A kịp chạy:
     Producer: mutex_unlock()

T=3: Worker B: mutex_lock()
     Worker B: count = 0 (đã dequeue rồi)
     Worker B: mutex_unlock()

T=4: Worker A: (WOKEN UP - có thể spurious)
     Worker A: [REMAINS INSIDE IF BLOCK]
     Worker A: items[head] = ??? → TRASH DATA hoặc CLOSED FD
     → CRASH
```

### 7.4 Minh họa với `while` (ĐÚNG)

```c
// src/thread_pool.c:118–126
while (queue->count == 0 && !queue->shutdown) {
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
    // Khi wake, KHUYẾN KHÍCH THỨC DẬY KIỂM TRA LẠI
    // Nếu count vẫn == 0 (spurious hoặc race):
    //   → quay lại while → ngủ tiếp
}
```

```
Timeline với while:

T=0: queue->count = 0
     Worker A: mutex_lock()
     Worker A: while (queue->count == 0) → TRUE
     Worker A: pthread_cond_wait(...)
     Worker A: [atomic unlock + sleep]
     
T=1: Producer: mutex_lock()
     Producer: count = 1
     Producer: signal(not_empty)
     Worker A: [wake, relock] → quay lên while

T=2: Worker A: while (queue->count == 0) → FALSE (count = 1)
     Worker A: thoát while → proceed bình thường
     ✅ SAFE

Spurious case:
T'=0: Worker A: mutex_lock()
T'=0: Worker A: while (count == 0) → TRUE → wait
T'=1: (NO SIGNAL — spurious wakeup)
T'=2: Worker A: wake → quay lên while
T'=3: Worker A: while (count == 0) → TRUE (vẫn rỗng)
T'=4: Worker A: while (...) → TRUE → ngủ lại
     ✅ SAFE — không làm gì sai
```

### 7.5 Tổng kết Spurious Wakeup

| Câu lệnh | Khi wake | Hành động |
|-----------|-----------|-----------|
| `if (condition)` | Thực thi body **ngay** | Có thể sai nếu condition vẫn false |
| `while (condition)` | Kiểm tra lại condition | Đúng trong mọi trường hợp |

**Quy tắc vàng:** Luôn dùng `while` cho `pthread_cond_wait()`.

---

## 8. Signal vs Broadcast

### 8.1 `signal()` — Đánh thức một thread

```c
// src/thread_pool.c:106
queue->count++;
pthread_cond_signal(&queue->not_empty);  // ← Chỉ 1 worker được wake
```

**Khi nào dùng signal:**
- Chỉ có 1 item mới được thêm → chỉ cần 1 worker
- Dùng signal giữ nguyên trạng thái các workers khác (tiết kiệm CPU)

### 8.2 `broadcast()` — Đánh thức TẤT CẢ threads

```c
// src/thread_pool.c:144
void socket_queue_shutdown(socket_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);   // ← WAKES ALL
    pthread_cond_broadcast(&queue->not_full);    // ← WAKES ALL
    pthread_mutex_unlock(&queue->mutex);
}
```

**Khi nào dùng broadcast:**
- Khi **tất cả** waiting threads cần được wake
- Shutdown: tất cả workers phải exit
- Khi condition được set cho **nhiều** threads cùng lúc

### 8.3 Thundering Herd

```
Signal() trong bình thường:
  Producer: enqueue(1 item)
  Signal: not_empty
  Result: 1 worker wake, nhận item, proceed
  
Nếu dùng broadcast() trong bình thường:
  Producer: enqueue(1 item)
  Broadcast: not_empty
  8 workers wake cùng lúc
  7 workers nhìn count == 0 → quay lại sleep
  → 7 context switches LÃNG PHÍ
  → THUNDERING HERD PROBLEM
```

Project dùng `signal()` trong enqueue/dequeue thường, `broadcast()` chỉ trong shutdown.

---

## 9. Graceful Shutdown — Tắt máy an toàn

### 9.1 Tại sao graceful shutdown quan trọng?

```
❌ ABRUPT SHUTDOWN (kill -9):
  → Worker đang gửi file 50MB → connection reset → client không nhận gì
  → FD leak → file descriptor không được close
  → Log không được flush → mất entries cuối
  → Heap memory không được free

✅ GRACEFUL SHUTDOWN:
  → Hoàn thành request hiện tại
  → Close connections đúng cách
  → Flush logs
  → Join all threads
  → Free all memory
```

### 9.2 Shutdown State Machine

```
server_stop() được gọi (SIGINT/SIGTERM)
  │
  ├─→ should_stop = 1
  │     (vòng accept loop check và exit)
  │
  ├─→ close(listen_fd)
  │     (accept() trả về -1 với errno tùy platform)
  │
  └─→ socket_queue_shutdown(queue)
          ├─→ shutdown = 1
          ├─→ broadcast(not_empty)   ← WAKE ALL WORKERS
          └─→ broadcast(not_full)

Các workers:
  ├─→ dequeue() thấy count == 0 + shutdown == 1
  ├─→ return QUEUE_CLOSED
  └─→ exit loop, return NULL

thread_pool_stop():
  ├─→ join all threads (đợi tất cả workers exit)
  └─→ free thread array

server_destroy():
  ├─→ close(listen_fd) (safety check)
  ├─→ access_log_close()
  └─→ socket_queue_destroy()
```

### 9.3 Worker exit path

```c
// src/thread_pool.c:22–34
static void *worker_main(void *arg) {
    worker_args_t *worker_args = arg;
    thread_pool_t *pool = worker_args->pool;
    free(worker_args);

    while (socket_queue_dequeue(pool->queue, &client_fd) == QUEUE_OK) {
        // Xử lý request...
        pool->handler(client_fd, pool->handler_context);
        // close(client_fd) được gọi bên trong handler
    }

    return NULL;  // QUEUE_CLOSED → thoát
}
```

Khi queue shutdown:
1. `broadcast(not_empty)` wake tất cả workers
2. Mỗi worker kiểm tra `count == 0 && shutdown == 1` → true
3. Return `QUEUE_CLOSED`
4. Thoát loop → return NULL → thread kết thúc
5. Main thread `join()` tất cả workers

### 9.4 Signal handler — an toàn

```c
// src/main.c:12–17
static void handle_signal(int signum) {
    (void)signum;
    if (active_server != NULL) {
        server_stop(active_server);  // Chỉ làm 1 việc: set flag + close socket
    }
}
```

**Tại sao signal handler phải đơn giản?**
- Signal handlers có giới hạn: chỉ async-signal-safe functions được gọi
- `printf()`, `malloc()`, `fprintf()` **không async-signal-safe** trong signal handler
- `server_stop()` chỉ gọi `write()` và `close()` → async-signal-safe
- `volatile sig_atomic_t` là kiểu atomic → đọc/ghi trong signal handler là an toàn

---

## 10. Deadlock Analysis

### 10.1 Kiểm tra: Có deadlock trong project không?

**Deadlock xảy ra khi:** A giữ Lock X, chờ Lock Y; B giữ Lock Y, chờ Lock X.

**Trong project:**

| Component | Lock held | Chờ gì |
|-----------|-----------|---------|
| Main thread | None | `accept()` block (không lock gì) |
| Main thread | `queue->mutex` (brief) | Nhả lock ngay sau enqueue |
| Worker | `queue->mutex` (brief) | Nhả lock ngay sau dequeue |

**Phân tích:**
- Main thread không bao giờ **chờ** worker (chỉ enqueue)
- Worker không bao giờ **chờ** main thread (chỉ dequeue)
- **Không có deadlock** — không có circular wait

### 10.2 Kịch bản deadlock tiềm ẩn (và cách tránh)

```
❌ DEADLOCK SCENARIO 1: Double lock
pthread_mutex_lock(&a);
pthread_mutex_lock(&a);  // DEADLOCK: cùng thread lock 2 lần (nếu không dùng recursive mutex)

✅ FIX: Dùng PTHREAD_MUTEX_NORMAL (default) → deadlock luôn, phát hiện sớm

❌ DEADLOCK SCENARIO 2: Lock trong signal handler
Signal handler: pthread_mutex_lock(&log->mutex)  // BLOCKS
Main thread: đang hold log->mutex
→ Nếu signal đến main thread → DEADLOCK

✅ FIX: Trong project, signal handler chỉ gọi server_stop(),
        không lock mutex nào

❌ DEADLOCK SCENARIO 3: Unlock nhầm thread khác
pthread_mutex_unlock(&other_thread_mutex);  // UNDEFINED BEHAVIOR

✅ FIX: Luôn unlock trong cùng thread đã lock
```

### 10.3 Điều kiện Coffman (4 điều kiện deadlock)

| Điều kiện | Trong project? | Giải thích |
|-----------|----------------|-----------|
| 1. Mutual Exclusion | Có | Mutex chỉ cho 1 thread access tại 1 thời điểm |
| 2. Hold and Wait | Không | Enqueue/Dequeue nhả lock ngay (không hold while waiting) |
| 3. No Preemption | Có | Mutex không thể bị preempt bởi thread khác |
| 4. Circular Wait | Không | Không có chain A→B→C→A |

→ **Deadlock KHÔNG XẢY RA** vì điều kiện 2 và 4 bị vi phạm.

---

## 11. Code Walkthrough: Producer Side

```c
// src/server.c:544–582
int server_run(server_t *server) {
    worker_context_t context = { .server = server };
    thread_pool_start(&server->pool, server->queue,
                      server->config.thread_count,
                      handle_client, &context);

    while (!server->should_stop) {
        // 1. ACCEPT
        int client_fd = accept(server->listen_fd, NULL, NULL);

        // 2. Handle accept error
        if (client_fd < 0) {
            if (errno == EINTR) continue;  // Signal interrupt → retry
            if (server->should_stop) break;
            continue;  // Other errors → continue
        }

        // 3. ENQUEUE (producer side)
        queue_result_t result = socket_queue_enqueue(server->queue, client_fd);

        // 4. BACKPRESSURE: queue full → reject immediately
        if (result == QUEUE_FULL) {
            response_result_t response = {0, 0};
            char client_ip[128];
            get_client_ip(client_fd, client_ip, sizeof(client_ip));
            send_simple_response(client_fd, NULL, 503,
                                "Service Unavailable\n", 0, &response);
            access_log_write(server->access_log, client_ip,
                            "-", 503, 0);
            close(client_fd);  // ← CRITICAL: phải close fd
        }
    }

    thread_pool_stop(&server->pool);  // ← Đợi workers
    return 0;
}
```

**Điểm quan trọng:**
1. `thread_pool_start()` được gọi **TRƯỚC** vòng lặp — workers sẵn sàng nhận job
2. Backpressure: `QUEUE_FULL` → từ chối ngay → `503` response
3. `thread_pool_stop()` đợi workers xong → graceful

---

## 12. Code Walkthrough: Consumer Side

```c
// src/thread_pool.c:90–135
queue_result_t socket_queue_dequeue(socket_queue_t *queue, int *client_fd) {
    // 1. LOCK
    pthread_mutex_lock(&queue->mutex);

    // 2. SLEEP if empty
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    // 3. CHECK: shutdown (queue empty + shutdown flag)
    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return QUEUE_CLOSED;  // → Worker exits
    }

    // 4. DEQUEUE item
    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    // 5. SIGNAL producer (dự phòng cho blocking enqueue)
    pthread_cond_signal(&queue->not_full);

    // 6. UNLOCK
    pthread_mutex_unlock(&queue->mutex);

    return QUEUE_OK;
}
```

---

## Tổng kết Level 3 — Quick Reference

```
┌─────────────────────────────────────────────────────────────┐
│ THREAD LIFECYCLE                                            │
│                                                             │
│  pthread_create()  → tạo thread, bắt đầu chạy function │
│  worker_main()      → vòng lặp: dequeue → handle → close │
│  pthread_join()     → main thread đợi workers kết thúc  │
│                                                             │
│  Mỗi thread có:                                            │
│  • Stack riêng (~8MB)                                     │
│  • Thread ID riêng                                        │
│  Chia sẻ: heap, code, FD table, global variables          │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ MUTEX — MUTUAL EXCLUSION                                   │
│                                                             │
│  pthread_mutex_lock()   → acquire                          │
│  pthread_mutex_unlock() → release                          │
│                                                             │
│  Dùng khi: thao tác SHARED DATA (queue count/head/tail) │
│  KHÔNG dùng khi: per-thread stack variables              │
│  Luôn unlock trong cùng thread đã lock                   │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ CONDITION VARIABLES — SLEEP HIỆU QUẢ                      │
│                                                             │
│  pthread_cond_wait(&cond, &mutex):                         │
│    1. unlock(mutex)                                       │
│    2. sleep (0% CPU)                                     │
│    3. [signal/broadcast]                                   │
│    4. wake → relock(mutex) → return                       │
│                                                             │
│  pthread_cond_signal()   → wake 1 thread                 │
│  pthread_cond_broadcast() → wake ALL threads               │
│                                                             │
│  Luôn dùng while, KHÔNG dùng if!                         │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ CIRCULAR BUFFER                                            │
│                                                             │
│  head = (head + 1) % capacity  ← dequeue                │
│  tail = (tail + 1) % capacity  ← enqueue                │
│  count tracker để phân biệt empty vs full (head==tail)   │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ GRACEFUL SHUTDOWN                                          │
│                                                             │
│  SIGINT/SIGTERM → signal handler                         │
│  server_stop()                                             │
│    → should_stop = 1                                       │
│    → close(listen_fd)         → accept() unblocks        │
│    → socket_queue_shutdown()                               │
│      → shutdown = 1                                        │
│      → broadcast(not_empty)  → ALL workers wake           │
│      → workers: thấy shutdown → return QUEUE_CLOSED      │
│    → pthread_join() all threads                            │
│    → free resources                                        │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ NO DEADLOCK                                                │
│                                                             │
│  Main thread: lock → enqueue → unlock (brief hold)      │
│  Workers: lock → dequeue → unlock (brief hold)            │
│  No circular wait: A→B→C→A pattern không tồn tại         │
│  No hold-and-wait: lock chỉ giữ trong microseconds        │
└─────────────────────────────────────────────────────────────┘
```
