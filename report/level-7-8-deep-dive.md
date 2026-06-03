# Level 7–8 Deep Dive — Signals, Process Lifecycle, Memory & Buffers

Level 7 và Level 8 là hai tầng cuối cùng trong System Programming. Level 7 giải quyết câu hỏi: **"Làm thế nào để quản lý vòng đời của một tiến trình và tắt máy server một cách an toàn?"** Level 8 giải quyết câu hỏi: **"Làm thế nào để quản lý bộ nhớ heap và stack, tránh overflow, và xử lý file hiệu quả?"**

---

## Mục lục

### Phần A: Signals và Process Lifecycle (Level 7)

1. [Signal — Khái niệm cơ bản](#1-signal--khái-niệm-cơ-bản)
2. [Signal Handler — Bắt tín hiệu](#2-signal-handler--bắt-tín-hiệu)
3. [Async-Signal-Safety — Chỉ gọi hàm nào trong handler](#3-async-signal-safety--chỉ-gọi-hàm-nào-trong-handler)
4. [Graceful Shutdown — Tắt máy an toàn](#4-graceful-shutdown--tắt-máy-an-toàn)
5. [volatile sig_atomic_t — Kiểu dữ liệu an toàn](#5-volatile-sig_atomict--kiểu-dữ-liệu-an-toàn)
6. [Server Lifecycle — Init → Run → Stop → Destroy](#6-server-lifecycle--init--run--stop--destroy)
7. [Signal Flow trong project](#7-signal-flow-trong-project)

### Phần B: Memory và Buffers (Level 8)

8. [Memory Architecture — Stack vs Heap](#8-memory-architecture--stack-vs-heap)
9. [Heap Allocation — malloc/calloc/realloc/free](#9-heap-allocation--malloccallocreallocfree)
10. [Buffer Overflow — Nguyên nhân và phòng thủ](#10-buffer-overflow--nguyên-nhân-và-phòng-thủ)
11. [memmove vs memcpy — Memory overlap](#11-memmove-vs-memcpy--memory-overlap)
12. [File Streaming — Chunked I/O](#12-file-streaming--chunked-io)
13. [Access Log — Thread-safe logging](#13-access-log--thread-safe-logging)

---

## Phần A: Signals và Process Lifecycle (Level 7)

---

## 1. Signal — Khái niệm cơ bản

### 1.1 Signal là gì?

```
┌──────────────────────────────────────────────────────────────┐
│  SIGNAL — Software Interrupt                                 │
│                                                              │
│  Kernel gửi signal đến process khi:                         │
│  • User nhấn Ctrl+C (SIGINT)                               │
│  • User gõ kill -15 <pid> (SIGTERM)                        │
│  • Process crash (SIGSEGV, SIGFPE)                         │
│  • Child process chết (SIGCHLD)                            │
│  • Timer expires (SIGALRM)                                 │
│                                                              │
│  Signal handler = function được gọi khi signal đến         │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 Các signals phổ biến

| Signal      | Số | Mặc định | Khi nào đến              |
| ----------- | --- | ----------- | --------------------------- |
| `SIGINT`  | 2   | Terminate   | Ctrl+C                      |
| `SIGTERM` | 15  | Terminate   | `kill <pid>` (graceful)   |
| `SIGKILL` | 9   | Terminate   | `kill -9 <pid>` (forced)  |
| `SIGSEGV` | 11  | Core dump   | Segmentation fault          |
| `SIGPIPE` | 13  | Ignore      | Write to closed pipe/socket |
| `SIGALRM` | 14  | Terminate   | Timer expires               |
| `SIGCHLD` | 17  | Ignore      | Child process exits         |

### 1.3 Signal trong project

```c
// src/main.c:86–88
active_server = &server;
signal(SIGINT, handle_signal);
signal(SIGTERM, handle_signal);
```

Project chỉ bắt `SIGINT` và `SIGTERM` — hai signals dùng để graceful shutdown.

---

## 2. Signal Handler — Bắt tín hiệu

### 2.1 Handler signature

```c
// src/main.c:12–17
static void handle_signal(int signum) {
    (void)signum;  // Không dùng, suppress unused warning
    if (active_server != NULL) {
        server_stop(active_server);
    }
}
```

**Signature phải là:** `void handler(int signum)`

- Compiler sẽ warn nếu sai signature
- `signum` là signal number (SIGINT=2, SIGTERM=15)

### 2.2 Tại sao dùng `signal()`?

```c
// Cú pháp đơn giản:
signal(SIGINT, handle_signal);

// Thay vì sigaction() (phức tạp hơn):
struct sigaction sa;
sa.sa_handler = handle_signal;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sigaction(SIGINT, &sa, NULL);
```

**Khi nào dùng signal():**

- Handler đơn giản, chỉ cần bắt signal
- Không cần control flags đặc biệt

**Khi nào dùng sigaction():**

- Cần kiểm soát flags (SA_RESTART, SA_SIGINFO)
- Cần block signals trong handler
- Cần union-based signal info

### 2.3 Signal handler execution context

```
┌──────────────────────────────────────────────────────────────┐
│  SIGNAL HANDLER EXECUTION CONTEXT                            │
│                                                              │
│  Main thread đang execute accept():                         │
│                                                              │
│  User nhấn Ctrl+C                                           │
│  → Kernel interrupt main thread                            │
│  → Main thread: SAVE STATE (registers, PC, stack pointer)  │
│  → Main thread: JUMP to signal handler                     │
│  → handle_signal() runs                                   │
│  → server_stop() called                                   │
│  → handler returns                                        │
│  → Main thread: RESTORE STATE                              │
│  → Main thread: resume from accept()                       │
│                                                              │
│  ⚠️ Signal handler có thể interrupt bất kỳ instruction nào  │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. Async-Signal-Safety — Chỉ gọi hàm nào trong handler

### 3.1 Vấn đề: Async-Signal-Safety

```
┌──────────────────────────────────────────────────────────────┐
│  ASYNC-SIGNAL-SAFETY                                        │
│                                                              │
│  Signal handler có thể chạy BẤT CỨ LÚC NÀO:               │
│  • Giữa một syscall (VD: giữa write() syscall)             │
│  • Khi accessing global data (VD: errno, malloc metadata)  │
│                                                              │
│  Nếu handler gọi hàm KHÔNG async-signal-safe:              │
│  → Hàm đó có thể bị interrupted bởi chính signal đó       │
│  → Race condition → CORRUPTED state → CRASH               │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 Các hàm async-signal-safe

```c
// ✅ ASYNC-SIGNAL-SAFE functions (dùng được trong handler):
//
// I/O:
//   write()      — Ghi vào fd
//   _exit()      — Exit ngay lập tức
//
// Memory:
//   _exit()      — Không flush buffers
//
// Signal:
//   signal()     — Set handler
//   _exit()      — Terminate
//
// NÊN DÙNG TRONG HANDLER:
//   write(STDOUT_FILENO, msg, len) — OK
//   server_stop() nếu KHÔNG gọi async-signal-unsafe bên trong
```

### 3.3 Các hàm async-signal-UNSAFE

```c
// ❌ KHÔNG DÙNG trong signal handler:

// I/O:
fprintf()    // Gọi malloc(), có thể crash
printf()     // Tương tự
fwrite()     // Tương tự
fopen()      // Gọi malloc()

// Memory:
malloc()     // Re-entrancy không đảm bảo
free()       // Có thể corrupt heap
realloc()    // Tương tự

// Threading:
pthread_mutex_lock() // Deadlock nếu handler gọi khi main thread đang hold

// Other:
syslog()     // Write to system log, async-signal-unsafe
openlog()    // Tương tự
```

### 3.4 `server_stop()` có async-signal-safe không?

```c
// src/server.c:server_stop()
void server_stop(server_t *server) {
    if (server == NULL) {
        return;
    }
    server->should_stop = 1;      // ✅ volatile sig_atomic_t write
    close(server->listen_fd);     // ✅ async-signal-safe
    socket_queue_shutdown(server->queue);  // ⚠️ Xem phân tích bên dưới
}
```

**Phân tích `socket_queue_shutdown()`:**

```c
// src/thread_pool.c:153–163
void socket_queue_shutdown(socket_queue_t *queue) {
    if (queue == NULL) {
        return;
    }
    pthread_mutex_lock(&queue->mutex);   // ⚠️ NOT async-signal-safe!
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);   // ⚠️ NOT async-signal-safe!
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}
```

**Nhưng đợi đã:** Tại sao code này vẫn chạy được?

```
Giải thích:
1. Signal handler được gọi từ MAIN THREAD (accept loop)
2. Signal handler gọi server_stop()
3. server_stop() gọi socket_queue_shutdown()
4. socket_queue_shutdown() gọi pthread_mutex_lock()

Nhưng KHI NÀO signal đến?
→ accept() đang blocking
→ Main thread đang ngủ trong kernel
→ Main thread không hold bất kỳ mutex nào
→ pthread_mutex_lock() trong handler KHÔNG deadlock

⚠️ CẢNH BÁO: Đây là edge case. Code hoạt động nhưng KHÔNG phải
   async-signal-safe theo spec. sigaction() với SA_SIGINFO
   là cách đúng để handle signals trong multi-threaded program.
```

---

## 4. Graceful Shutdown — Tắt máy an toàn

### 4.1 Tại sao graceful shutdown quan trọng?

```
┌──────────────────────────────────────────────────────────────┐
│  ABRUPT SHUTDOWN vs GRACEFUL SHUTDOWN                       │
│                                                              │
│  ABRUPT (kill -9 / crash):                                 │
│  • Worker đang gửi file 50MB → connection reset            │
│  • File descriptor không close → FD leak                     │
│  • Log không flush → entries cuối mất                       │
│  • Memory leak → heap không free                            │
│  • Request đang xử lý → client nhận nothing                │
│                                                              │
│  GRACEFUL (SIGTERM / SIGINT):                               │
│  • .should_stop = 1 → accept loop exit                      │
│  • close(listen_fd) → accept() unblocks                    │
│  • broadcast(not_empty) → ALL workers wake                  │
│  • Workers complete current request → close connection      │
│  • pthread_join() → đợi all workers exit                  │
│  • Log flush → access_log_write complete                  │
│  • free(all memory) → clean exit                          │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 Shutdown sequence

```
Timeline khi SIGINT đến:

T=0:  Main thread: accept() blocking
      Workers: dequeue() → handle_client() → sending file
      Users: browsing website normally

T=1:  User nhấn Ctrl+C
      Kernel: gửi SIGINT đến process
      Main thread: interrupted, handle_signal() runs
      → server_stop()
      → should_stop = 1
      → close(listen_fd)  → accept() unblocks với errno

T=2:  Main thread: accept() returns -1
      Main thread: should_stop = 1 → break accept loop
      Main thread: thread_pool_stop()

T=3:  socket_queue_shutdown(queue)
      → shutdown = 1
      → broadcast(not_empty) → ALL workers WAKE UP
      → broadcast(not_full) → (dự phòng)

T=4:  Workers wake up
      Workers: while (count == 0 && shutdown == 1)
      Workers: → return QUEUE_CLOSED
      Workers: → exit loop → return NULL

T=5:  pthread_join(all threads)
      Main thread: blocked, waiting for workers
      Workers: one by one finish and exit

T=6:  All threads joined
      Main thread: continue → server_destroy()
      → access_log_close() → fflush + fclose
      → socket_queue_destroy() → free all
      → exit(0)
```

### 4.3 Không graceful shutdown = crashes

```
BUG 1: Worker bị kill trước khi close(client_fd)
  → FD leak → hết FDs sau 1024 requests
  → "Too many open files"

BUG 2: Log không flush
  → access_log_write() bị interrupt
  → fprintf() buffer không write ra disk
  → Mất log entries

BUG 3: Queue không broadcast
  → Workers ngủ vĩnh viễn trong dequeue()
  → pthread_join() NEVER returns → hang forever

BUG 4: File descriptor không close
  → accept() tạo fd mới, main loop exit
  → 1 FD mỗi SIGINT → leak
```

---

## 5. `volatile sig_atomic_t` — Kiểu dữ liệu an toàn

### 5.1 `volatile` — Ngăn compiler optimization

```c
// include/server.h:14
volatile sig_atomic_t should_stop;
```

```
Vấn đề:
  while (!server->should_stop) {
      accept();
  }

Compiler optimization:
  Compiler thấy should_stop = 0 → "永远不会变" (永远不会变)
  → Convert thành: while (true) { accept(); }
  → Bỏ qua check should_stop hoàn toàn!

volatile ngăn chặn optimization:
  → Mỗi lần đọc should_stop là MEMORY READ THỰC SỰ
  → Không cache trong register
  → Signal handler write → main thread thấy ngay
```

### 5.2 `sig_atomic_t` — Atomic read/write

```c
/*
sig_atomic_t = integer type có thể được read/write
              như một operation KHÔNG THỂ chia cắt (atomic)

Trên modern x86-64:
  Read/write int = tự động atomic (nếu aligned)
  sig_atomic_t đảm bảo:
  • Single instruction
  • Không bị torn read/write (half-written)
*/

// Signal handler: write
server->should_stop = 1;  // Atomic write

// Main thread: read
while (!server->should_stop) {  // Atomic read
    accept();
}
```

### 5.3 Kết hợp: `volatile sig_atomic_t`

```c
// include/server.h:14
volatile sig_atomic_t should_stop;

/*
Tại sao CẢ HAI?

volatile:
  • Ngăn compiler cache giá trị
  • Mỗi access = memory access thực sự

sig_atomic_t:
  • Đảm bảo read/write là atomic operation
  • Không bị torn (half-written)

Nếu CHỈ dùng volatile:
  int should_stop;
  while (!server->should_stop)  // Compiler có thể optimize!

Nếu CHỈ dùng sig_atomic_t:
  volatile sig_atomic_t should_stop;
  // ✅ Đúng
*/

// Anti-patterns:

// ❌ SAI: int* (không phải sig_atomic_t)
volatile int *p = &should_stop;
*p = 1;  // Not guaranteed atomic

// ❌ SAI: long (không phải sig_atomic_t)
volatile long big_value;  // Trên 32-bit, long = 64-bit, có thể torn
```

---

## 6. Server Lifecycle — Init → Run → Stop → Destroy

### 6.1 State Machine

```
┌──────────────────────────────────────────────────────────────┐
│  SERVER LIFECYCLE STATE MACHINE                            │
│                                                              │
│  ┌──────────┐                                              │
│  │   INIT   │                                              │
│  └────┬─────┘                                              │
│       │ server_init()                                     │
│       ↓                                                    │
│  ┌──────────┐                                              │
│  │  READY   │ ← signal(SIGINT), signal(SIGTERM)          │
│  └────┬─────┘                                              │
│       │ server_run()                                      │
│       ↓                                                    │
│  ┌──────────┐                                              │
│  │ RUNNING  │ ← accept loop chạy                        │
│  └────┬─────┘                                              │
│       │ SIGINT/SIGTERM → server_stop()                  │
│       ↓                                                    │
│  ┌──────────┐                                              │
│  │ STOPPING │ ← join threads, cleanup                   │
│  └────┬─────┘                                              │
│       │ server_destroy()                                 │
│       ↓                                                    │
│  ┌──────────┐                                              │
│  │  EXIT    │ ← return exit_code                        │
│  └──────────┘                                              │
└──────────────────────────────────────────────────────────────┘
```

### 6.2 `server_init()` — Khởi tạo

```c
// src/server.c:466–558
int server_init(server_t *server, const server_config_t *config) {
    memset(server, 0, sizeof(*server));
    server->config = *config;
    server->should_stop = 0;
    server->listen_fd = -1;

    // 1. Open access log
    if (access_log_open(&server->access_log, config->access_log) != 0) {
        return -1;
    }

    // 2. Create socket queue
    server->queue = socket_queue_create(config->queue_capacity);
    if (server->queue == NULL) {
        access_log_close(server->access_log);
        return -1;
    }

    // 3. Create listening socket
    server->listen_fd = create_listening_socket(config);
    if (server->listen_fd < 0) {
        access_log_close(server->access_log);
        socket_queue_destroy(server->queue);
        return -1;
    }

    return 0;
}
```

### 6.3 `server_run()` — Vòng lặp chính

```c
// src/server.c:560–618
int server_run(server_t *server) {
    worker_context_t context = { .server = server };

    // 1. Start thread pool (producers)
    if (thread_pool_start(&server->pool, server->queue,
                          server->config.thread_count,
                          handle_client, &context) != 0) {
        return 1;
    }

    // 2. Accept loop (producer loop)
    while (!server->should_stop) {
        int client_fd = accept(server->listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (server->should_stop) break;
            continue;
        }

        // Enqueue (producer action)
        queue_result_t result = socket_queue_enqueue(server->queue, client_fd);
        if (result == QUEUE_FULL) {
            send_simple_response(client_fd, NULL, 503, "Service Unavailable\n", 0, &response);
            close(client_fd);
        }
    }

    // 3. Stop thread pool (graceful shutdown)
    thread_pool_stop(&server->pool);
    return 0;
}
```

### 6.4 `server_stop()` — Signal handler callback

```c
// src/server.c:620–631
void server_stop(server_t *server) {
    if (server == NULL) {
        return;
    }
    server->should_stop = 1;       // Set flag
    close(server->listen_fd);      // Unblock accept()
    socket_queue_shutdown(server->queue);  // Wake all workers
}
```

### 6.5 `server_destroy()` — Dọn dẹp

```c
// src/server.c:633–655
void server_destroy(server_t *server) {
    if (server == NULL) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    if (server->access_log != NULL) {
        access_log_close(server->access_log);
        server->access_log = NULL;
    }

    if (server->queue != NULL) {
        socket_queue_destroy(server->queue);
        server->queue = NULL;
    }
}
```

---

## 7. Signal Flow trong project

### 7.1 Từ Ctrl+C đến exit

```
┌──────────────────────────────────────────────────────────────┐
│  FULL SIGNAL FLOW                                            │
│                                                              │
│  1. User nhấn Ctrl+C                                       │
│                                                              │
│  2. Terminal driver gửi SIGINT đến foreground process group │
│                                                              │
│  3. Kernel deliver SIGINT đến httpd process                 │
│                                                              │
│  4. CPU: Save execution context của main thread              │
│     • PC, registers, stack pointer                          │
│     • Signal mask, errno                                   │
│                                                              │
│  5. CPU: JUMP to handle_signal()                           │
│                                                              │
│  6. handle_signal(int signum):                             │
│     • signum = 2 (SIGINT)                                 │
│     • server_stop(server)                                  │
│       → should_stop = 1                                    │
│       → close(listen_fd)                                  │
│       → socket_queue_shutdown(queue)                       │
│         → shutdown = 1                                     │
│         → broadcast(not_empty)                             │
│         → broadcast(not_full)                               │
│                                                              │
│  7. Return from handler                                    │
│                                                              │
│  8. CPU: Restore context                                   │
│                                                              │
│  9. accept() returns -1 (listen_fd closed)                  │
│     errno = EBADF (bad file descriptor)                   │
│                                                              │
│ 10. accept loop: should_stop == 1 → break                  │
│                                                              │
│ 11. thread_pool_stop():                                    │
│     → join all workers                                     │
│                                                              │
│ 12. server_destroy():                                       │
│     → close(listen_fd)                                     │
│     → access_log_close() → fflush()                        │
│     → socket_queue_destroy() → free memory                 │
│                                                              │
│ 13. main() returns exit_code                               │
│     → Process exits cleanly                               │
└──────────────────────────────────────────────────────────────┘
```

### 7.2 Edge cases trong signal handling

```
EDGE CASE 1: Signal trước khi server_run()
  • main(): signal() → register handler
  • SIGINT arrives BEFORE server_run()
  • handle_signal() → server_stop()
  • should_stop = 1, nhưng server chưa start
  • server_run() → while (!should_stop) → exit ngay
  → OK, không crash

EDGE CASE 2: Multiple SIGINT
  • SIGINT #1 arrives
  • handle_signal() → server_stop()
  • SIGINT #2 arrives DURING handle_signal()
  • POSIX: Handler đang chạy = blocked by default
  • SIGINT #2 queued, execute sau khi handler return
  • server_stop() gọi 2 lần → OK (idempotent)
  → OK, không deadlock

EDGE CASE 3: SIGINT vs SIGTERM
  • Cả hai gọi cùng handler
  • Handler check active_server → gọi server_stop()
  • signal(SIGINT) và signal(SIGTERM) cùng handler
  → OK

EDGE CASE 4: Signal during pthread_mutex_lock()
  • Nếu signal đến KHI main thread đang hold mutex
  • Handler gọi socket_queue_shutdown()
  • socket_queue_shutdown() gọi pthread_mutex_lock()
  • DEADLOCK (nếu mutex không phải recursive)
  • Trong project: signal đến khi accept() blocking
  → accept() không hold mutex → safe
  → OK
```

---

## Phần B: Memory và Buffers (Level 8)

---

## 8. Memory Architecture — Stack vs Heap

### 8.1 Process memory layout

```
┌──────────────────────────────────────────────────────────────┐
│  PROCESS MEMORY LAYOUT (Linux x86-64)                        │
│                                                              │
│  0xFFFFFFFFFFFFFFFF                                          │
│  ┌────────────────┐                                          │
│  │  Kernel Space  │ ← Kernel code/data (inaccessible)      │
│  └────────────────┘                                          │
│  0xFFFFFFFF80000000                                          │
│  ┌────────────────┐                                          │
│  │     Stack      │ ← Local vars, function params           │
│  │  (grows down)  │ ← ~8MB default per thread              │
│  └────────────────┘                                          │
│                                                              │
│  [ unused memory ]                                           │
│                                                              │
│  ┌────────────────┐                                          │
│  │      Heap      │ ← malloc/calloc/realloc                │
│  │  (grows up)    │ ← dynamic allocations                  │
│  └────────────────┘                                          │
│  ┌────────────────┐                                          │
│  │      BSS       │ ← uninitialized globals                │
│  └────────────────┘                                          │
│  ┌────────────────┐                                          │
│  │      Data       │ ← initialized globals                 │
│  └────────────────┘                                          │
│  ┌────────────────┐                                          │
│  │      Text       │ ← code (read-only)                   │
│  └────────────────┘                                          │
│  0x0000000000400000                                          │
└──────────────────────────────────────────────────────────────┘
```

### 8.2 Stack trong project

```c
// src/server.c:382–406 — Mỗi worker có stack riêng
void handle_client(int client_fd, void *context) {
    char buffer[READ_BUFFER_SIZE];        // 8KB stack
    char request_line[256];              // 256 bytes stack
    char client_ip[128];                 // 128 bytes stack
    http_request_t request;             // ~1KB stack
    file_info_t info;                  // ~512 bytes stack

    // Tất cả biến này là PRIVATE cho mỗi thread
    // Không cần mutex vì stack không share giữa threads
}
```

### 8.3 Heap trong project

```c
// src/thread_pool.c — Heap allocations

// Queue buffer
int *items = calloc(capacity, sizeof(int));  // Heap

// Thread IDs
pthread_t *threads = calloc(thread_count, sizeof(*threads));  // Heap

// Worker args
worker_args_t *args = malloc(sizeof(*args));  // Heap

// String builder (files.c)
char *data = malloc(initial_capacity);  // Heap

// Access log
access_log_t *log = calloc(1, sizeof(*log));  // Heap
```

### 8.4 Stack vs Heap comparison

```
┌──────────────────────────────────────────────────────────────┐
│  STACK vs HEAP                                              │
│                                                              │
│  STACK:                                                      │
│  • Tự động allocation khi function called                  │
│  • Tự động deallocation khi function returns               │
│  • Kích thước FIXED tại compile time (VD: char buf[8192]) │
│  • Tốc độ: NHANH (pointer decrement)                       │
│  • Size limit: ~8MB per thread (configurable)              │
│  • KHÔNG share giữa threads                                │
│  • Dùng cho: local variables, function params              │
│                                                              │
│  HEAP:                                                       │
│  • Manual allocation: malloc/calloc/realloc                │
│  • Manual deallocation: free                             │
│  • Kích thước động (runtime)                              │
│  • Tốc độ: CHẬM hơn (malloc metadata lookup)             │
│  • Size limit: system RAM + swap                          │
│  • SHARE giữa threads                                     │
│  • Dùng cho: dynamic data, large buffers, shared state   │
└──────────────────────────────────────────────────────────────┘
```

---

## 9. Heap Allocation — malloc/calloc/realloc/free

### 9.1 `malloc()` — Allocate raw memory

```c
// src/thread_pool.c:187
worker_args_t *args = malloc(sizeof(*args));

/*
malloc(size):
• Allocates 'size' bytes from heap
• Returns pointer to allocated memory
• Memory is UNINITIALIZED (contains garbage)
• Returns NULL if allocation fails

sizeof(*args) = sizeof(worker_args_t)
• Đúng vì: dùng size của type được allocated
• Nếu type thay đổi, sizeof tự update
• An toàn hơn: malloc(sizeof(worker_args_t))
*/
```

### 9.2 `calloc()` — Allocate zero-initialized memory

```c
// src/thread_pool.c:172
pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));

/*
calloc(nmemb, size):
• Allocates 'nmemb * size' bytes
• Returns POINTER TO ZERO-INITIALIZED memory
• Useful for arrays and structs

So sánh:
malloc(n)     → uninitialized (faster)
calloc(1, n)  → zero-initialized (safer)

Trong project:
• threads array: calloc để zero-initialized
• access_log: calloc để zero-initialized
• file_info: memset(0) sau allocation
*/
```

### 9.3 `free()` — Deallocate memory

```c
// src/thread_pool.c:30
free(worker_args);

/*
free(ptr):
• Returns memory to heap
• ptr phải là pointer returned by malloc/calloc/realloc
• Nếu ptr = NULL → no-op

SAI PHỔ BIẾN:

❌ Double free:
  free(ptr);
  free(ptr);  // CRASH: free() twice

❌ Use after free:
  free(ptr);
  ptr[0] = 'a';  // CORRUPTED: accessing freed memory

❌ Free stack pointer:
  char buf[100];
  free(buf);  // CRASH: buf is on stack
*/
```

### 9.4 `realloc()` — Resize allocation

```c
// src/files.c:58
grown = realloc(builder->data, new_capacity);

/*
realloc(ptr, new_size):
• Changes size of existing allocation
• Returns NEW pointer (may move data)
• Copies old data to new location if moved
• If ptr = NULL → acts like malloc()
• If new_size = 0 → acts like free(), returns NULL

AN TOÀN KHI DÙNG:

char *new_data = realloc(old_data, new_size);
if (new_data == NULL) {
    // old_data still valid!
    // handle error
    free(old_data);
    return -1;
}
old_data = new_data;  // Update pointer
*/
```

### 9.5 Memory leak prevention

```c
// Trong project: tất cả allocations đều có corresponding free()

// thread_pool_stop():
void thread_pool_stop(thread_pool_t *pool) {
    free(pool->threads);        // ✓ Free thread array
    pool->threads = NULL;       // ✓ Null to prevent double-free
}

// socket_queue_destroy():
void socket_queue_destroy(socket_queue_t *queue) {
    if (queue == NULL) return;
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    free(queue->items);        // ✓ Free array
    free(queue);               // ✓ Free struct
}

// server_destroy():
void server_destroy(server_t *server) {
    if (server->access_log) access_log_close(server->access_log);
    if (server->queue) socket_queue_destroy(server->queue);
}
```

---

## 10. Buffer Overflow — Nguyên nhân và phòng thủ

### 10.1 Buffer Overflow là gì?

```
┌──────────────────────────────────────────────────────────────┐
│  BUFFER OVERFLOW                                             │
│                                                              │
│  Writing PAST the end of a buffer → Overwrites adjacent     │
│  memory → UNDEFINED BEHAVIOR → CRASH hoặc SECURITY BUG     │
│                                                              │
│  Ví dụ:                                                     │
│  char buf[4];                                               │
│  strcpy(buf, "Hello");  // 6 bytes (including \0)           │
│                                                              │
│  Stack layout:                                              │
│  [buf[0]] [buf[1]] [buf[2]] [buf[3]] [saved EBP] [return] │
│                                                              │
│  After strcpy:                                              │
│  [H] [e] [l] [l] [o] [\0] [saved EBP] [return]            │
│                                          ↑ OVERWRITTEN!      │
│  → Saved return address corrupted → SEGFAULT               │
└──────────────────────────────────────────────────────────────┘
```

### 10.2 `sprintf()` vs `snprintf()`

```c
// ❌ NGUY HIỂM: sprintf() — NO BOUNDS CHECK
char buf[32];
sprintf(buf, "%s", user_input);  // If user_input > 31 bytes → OVERFLOW

// ✅ AN TOÀN: snprintf() — WITH BOUNDS CHECK
char buf[32];
snprintf(buf, sizeof(buf), "%s", user_input);
// sizeof(buf) = 32 → maximum bytes written
// If truncated, return = number that WOULD be written
```

### 10.3 `snprintf()` trong project

```c
// src/server.c:298–306
char headers[RESPONSE_BUFFER_SIZE];
int header_len = snprintf(headers, sizeof(headers),
    "%s 200 OK\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    ...
```

```c
// src/files.c:452
if (snprintf(joined, sizeof(joined), "%s/%s", doc_root, relative_path)
    >= (int)sizeof(joined)) {
    return FILE_RESULT_ERROR;  // Path quá dài → truncate
}
```

### 10.4 `strcpy()` vs `strncpy()` vs `memcpy()`

```c
// ❌ strcpy() — NO BOUNDS CHECK
strcpy(dest, src);  // OVERFLOW if src > dest

// ⚠️ strncpy() — PARTIAL, POTENTIALLY DANGEROUS
strncpy(dest, src, n);
// Problems:
// 1. Does NOT guarantee null-termination if src[n-1] != '\0'
// 2. Pads with zeros if src shorter than n (slow)
// 3. May truncate without warning

// ✅ memcpy() — EXACT COPY
memcpy(dest, src, n);
// Just copies n bytes
// dest must have space for n bytes
// Does NOT add null terminator

// ✅ strncpy() CORRECT USAGE (trong project KHÔNG dùng):
strncpy(dest, src, n - 1);
dest[n - 1] = '\0';  // MANUAL null-terminate

// ✅ copy_bounded() trong project
// src/http.c:7–14
static void copy_bounded(char *dest, size_t dest_size, const char *src, size_t src_len) {
    size_t copy_len = src_len;
    if (copy_len >= dest_size) {
        copy_len = dest_size - 1;  // Leave room for \0
    }
    memcpy(dest, src, copy_len);   // Copy exact bytes
    dest[copy_len] = '\0';        // ALWAYS null-terminate
}
```

### 10.5 Integer overflow prevention

```c
// src/files.c:41–43
if (builder->length + additional + 1 > builder->capacity) {
    // Would overflow size_t
}

// src/http.c:83–85
if (parsed > (SIZE_MAX - (size_t)(*cursor - '0')) / 10U) {
    return 0;  // Integer overflow → reject
}
```

---

## 11. `memmove()` vs `memcpy()` — Memory overlap

### 11.1 Sự khác biệt

```
┌──────────────────────────────────────────────────────────────┐
│  memcpy() vs memmove()                                       │
│                                                              │
│  memcpy(dest, src, n):                                       │
│  • Copy n bytes from src to dest                           │
│  • UNDEFINED if regions OVERLAP                             │
│  • FASTER (no overlap check)                               │
│  • Dùng khi CHẮC CHẮN không overlap                       │
│                                                              │
│  memmove(dest, src, n):                                     │
│  • Copy n bytes from src to dest                          │
│  • DEFINED even if regions OVERLAP                        │
│  • SLOWER (may copy to temp buffer)                       │
│  • Dùng khi CÓ THỂ overlap                                │
└──────────────────────────────────────────────────────────────┘
```

### 11.2 Khi nào dùng `memmove()`

```c
// src/server.c:113
memmove(buffer, buffer + header_length, *buffered - header_length);

/*
Buffer BEFORE memmove():
[ GET / HTTP/1.1\r\n\r\n剩余数据 ][header_length bytes][...]
                         ↑                        ↑
                      buffer+header_length       buffer

Buffer AFTER memmove():
[GET / HTTP/1.1\r\n\r\n][剩余数据  ][...]
                        buffer          buffer + (buffered - header_length)

regions OVERLAP (src > dest)
→ PHẢI dùng memmove() không phải memcpy()

Overlap case:
src = buffer + header_length
dest = buffer
src > dest → regions overlap forward
→ memmove() handles correctly
*/
```

### 11.3 Implementation của memmove()

```c
/*
memmove() có thể copy từ đầu hoặc cuối tùy overlap direction:

If src > dest (copy forward):
  dest[0] = src[0]
  dest[1] = src[1]
  ...

If src < dest (copy backward):
  dest[n-1] = src[n-1]
  dest[n-2] = src[n-2]
  ...

→ Không có undefined behavior
*/

// memcpy() không handle overlap → faster but unsafe
// memmove() handle overlap → slightly slower but safe
```

---

## 12. File Streaming — Chunked I/O

### 12.1 Vấn đề: File lớn

```
┌──────────────────────────────────────────────────────────────┐
│  FILE STREAMING vs LOAD ENTIRE FILE                         │
│                                                              │
│  LOAD ENTIRE FILE:                                          │
│  char *data = malloc(file_size);                            │
│  fread(data, 1, file_size, file);                         │
│  send(fd, data, file_size, 0);                             │
│  free(data);                                               │
│                                                              │
│  Problems:                                                  │
│  • File 4GB → malloc 4GB → OOM on 32-bit                   │
│  • File larger than RAM → swap to disk → SLOW              │
│  • Memory fragmentation                                    │
│                                                              │
│  STREAMING (Chunked I/O):                                   │
│  char buf[8192];                                           │
│  while (remaining > 0) {                                   │
│      n = fread(buf, 1, sizeof(buf), file);               │
│      send(fd, buf, n, 0);                                 │
│      remaining -= n;                                      │
│  }                                                         │
│                                                              │
│  Benefits:                                                  │
│  • Memory usage = chunk size (8KB)                         │
│  • Works with files of ANY size                           │
│  • Sequential reads = optimal disk I/O                    │
└──────────────────────────────────────────────────────────────┘
```

### 12.2 Chunked streaming trong code

```c
// src/server.c:324–336
while (remaining > 0) {
    size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

    // Đọc chunk từ file
    size_t got = fread(buffer, 1, want, file);
    if (got == 0) {
        fclose(file);
        return -1;  // Lỗi đọc
    }

    // Gửi chunk qua socket
    if (send_all(fd, buffer, got) != 0) {
        fclose(file);
        return -1;  // Client đóng connection
    }

    remaining -= got;
}
```

### 12.3 Buffer size considerations

```c
#define READ_BUFFER_SIZE 8192  // 8KB

char buffer[READ_BUFFER_SIZE];

/*
Buffer size = 8KB:
• Bội số của disk sector size (512 bytes)
• Phù hợp với TCP buffer sizes
• Đủ lớn cho efficiency
• Đủ nhỏ để không waste memory

Too small (512 bytes):
• Nhiều syscall hơn (chậm)
• Too large (1MB):
• Memory waste nếu file nhỏ
• 8KB là sweet spot cho general-purpose
*/
```

### 12.4 `fread()` vs `read()`

```
fread(buffer, 1, n, file):
• Buffered I/O (C library)
• Uses internal buffer (thường 4KB-8KB)
• read() syscall chỉ khi internal buffer hết
• Better for sequential reads
• Returns số bytes actually read

read(fd, buffer, n):
• Direct syscall
• Mỗi call = syscall overhead
• Dùng cho:
  - Non-blocking I/O
  - Precise control
  - Raw device I/O
```

### 12.5 File streaming với Range requests

```c
// src/server.c:278–281
if (range.partial && fseeko(file, (off_t)range.start, SEEK_SET) != 0) {
    fclose(file);
    return send_simple_response(fd, request, 500, "Internal Server Error\n", 0, result);
}

/*
fseeko(file, offset, SEEK_SET):
• Position to byte 'offset' from start of file
• offset = 64-bit (off_t)
• Dùng cho Range requests (206 Partial Content)

Lý do dùng fseeko() thay vì fseek():
• fseek() có giới hạn 2GB hoặc 4GB trên 32-bit systems
• fseeko() dùng off_t (64-bit) → hỗ trợ file > 4GB
*/
```

---

## 13. Access Log — Thread-safe logging

### 13.1 Access log format (NCSA Common Log Format)

```c
// src/log.c:68–69
fprintf(log->file, "%s - - [%s] \"%s\" %d %zu\n",
        client_ip, timestamp, request_line, status_code, bytes_sent);

/*
Output example:
127.0.0.1 - - [02/Jun/2026:10:30:00 +0700] "GET /index.html HTTP/1.1" 200 1234
192.168.1.1 - - [02/Jun/2026:10:30:01 +0700] "GET /style.css HTTP/1.1" 200 567

Format:
host ident authuser [timestamp] "request" status bytes
• host: client IP
• ident: "-"
• authuser: "-"
• timestamp: Apache format
• request: full request line
• status: HTTP status code
• bytes: bytes sent (không tính headers)
*/
```

### 13.2 Thread-safe logging với mutex

```c
// src/log.c:51–74
void access_log_write(access_log_t *log, const char *client_ip,
                      const char *request_line, int status_code, size_t bytes_sent) {
    time_t now;
    struct tm local_time;
    char timestamp[64];

    // Format timestamp
    now = time(NULL);
    localtime_r(&now, &local_time);  // Reentrant version
    strftime(timestamp, sizeof(timestamp), "%d/%b/%Y:%H:%M:%S %z", &local_time);

    // Lock trước khi ghi
    pthread_mutex_lock(&log->mutex);

    // Ghi log
    fprintf(log->file, "%s - - [%s] \"%s\" %d %zu\n",
            client_ip, timestamp, request_line, status_code, bytes_sent);

    // Flush ngay lập tức
    fflush(log->file);

    // Unlock
    pthread_mutex_unlock(&log->mutex);
}
```

### 13.3 Tại sao cần mutex?

```
┌──────────────────────────────────────────────────────────────┐
│  RACE CONDITION KHÔNG CÓ MUTEX                              │
│                                                              │
│  Worker 1: fprintf(log->file, "%s...", ip1)                │
│  Worker 2: fprintf(log->file, "%s...", ip2)                │
│                                                              │
│  Possible output:                                           │
│  127.0.0.1 - 192.168.1.1 - ...                             │
│  → INTERLEAVED lines!                                      │
│                                                              │
│  Với mutex:                                                 │
│  Worker 1: lock → fprintf(ip1) → fflush → unlock           │
│  Worker 2: [blocked, waiting for lock]                     │
│  Worker 1: ✓ complete                                     │
│  Worker 2: lock → fprintf(ip2) → fflush → unlock           │
│  Worker 2: ✓ complete                                     │
│  → Separated lines                                        │
└──────────────────────────────────────────────────────────────┘
```

### 13.4 `localtime_r()` vs `localtime()`

```c
// ❌ localtime() — NOT thread-safe
struct tm *result = localtime(&now);
// Uses STATIC internal buffer
// If another thread calls localtime() → buffer overwritten
// Result is SHARED between threads

// ✅ localtime_r() — Thread-safe
struct tm local_time;
localtime_r(&now, &local_time);
// Thread provides own buffer (stack variable)
// Each thread has independent storage
// Thread-safe
```

### 13.5 `fflush()` — Flush immediately

```c
// src/log.c:71
fflush(log->file);

/*
fprintf() uses BUFFERED I/O:
• Data goes to library buffer (user space)
• Periodically written to kernel/disk
• fflush() forces immediate write to kernel

Tại sao cần flush trong access log?
• Server crash → buffered data LOST
• With fflush():
  → Every log entry written to kernel
  → Kernel may still buffer (fsync() for guarantees)
  → But much safer than no flush

Trade-off:
• fflush() after EVERY write → SLOWER
• No fflush() → Risk of losing logs
• Project uses fflush() = correctness > performance
*/
```

### 13.6 `fopen()` mode — Append mode

```c
// src/log.c:26
created->file = fopen(path, "a");

/*
"a" mode = APPEND:
• Every write goes to END of file
• Existing content preserved
• If file doesn't exist → created
• If file exists → new writes appended

Alternative:
"w" mode = WRITE:
• File truncated to 0
• All existing content LOST
• Dangerous for logs!

"r+" mode = READ-WRITE:
• Random access
• Not suitable for appending logs
*/
```

---

## Tổng kết Level 7–8 — Quick Reference

```
┌─────────────────────────────────────────────────────────────┐
│ SIGNAL HANDLING                                            │
│                                                             │
│  signal(SIGINT, handler)                                  │
│  signal(SIGTERM, handler)                                  │
│                                                             │
│  Handler chỉ gọi async-signal-safe functions:            │
│  → write(), _exit()                                       │
│  → server_stop() (nếu không hold mutex)                  │
│  → should_stop = 1 (volatile sig_atomic_t)              │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ GRACEFUL SHUTDOWN SEQUENCE                                 │
│                                                             │
│  1. should_stop = 1                                       │
│  2. close(listen_fd)         → accept() unblocks        │
│  3. broadcast(not_empty)      → ALL workers wake          │
│  4. Workers exit loop         → return QUEUE_CLOSED      │
│  5. pthread_join()           → wait for workers          │
│  6. fflush(log) + fclose()  → log flushed              │
│  7. free(all)                → memory released          │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ volatile sig_atomic_t                                      │
│                                                             │
│  volatile:    Ngăn compiler optimize away reads           │
│  sig_atomic_t: Đảm bảo atomic read/write                  │
│                                                             │
│  Dùng cho: flags được write trong handler,                 │
│           read trong main thread                           │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ STACK vs HEAP                                              │
│                                                             │
│  Stack:  local vars, auto allocation, ~8MB/thread         │
│  Heap:   malloc/calloc/realloc/free, shared, dynamic      │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ BUFFER OVERFLOW PREVENTION                                 │
│                                                             │
│  ✅ snprintf() thay vì sprintf()                          │
│  ✅ copy_bounded() thay vì strcpy()                       │
│  ✅ sizeof(dest) kiểm tra trước khi copy                 │
│  ✅ Kiểm tra integer overflow trước khi allocate         │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ FILE STREAMING                                             │
│                                                             │
│  char buf[8192];                                          │
│  while (remaining > 0) {                                  │
│      got = fread(buf, 1, want, file);                   │
│      send_all(fd, buf, got);                            │
│      remaining -= got;                                   │
│  }                                                       │
│                                                             │
│  → Memory usage = chunk size (8KB)                        │
│  → Works with files of ANY size                         │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ THREAD-SAFE LOGGING                                        │
│                                                             │
│  pthread_mutex_lock() → fprintf() → fflush() → unlock()  │
│                                                             │
│  Dùng localtime_r() (reentrant) thay vì localtime()      │
│  Dùng fflush() để đảm bảo log không mất khi crash       │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ MEMMOVE vs MEMCPY                                         │
│                                                             │
│  memmove(): Khi regions CÓ THỂ overlap                  │
│  memcpy():   Khi regions CHẮC CHẮN không overlap          │
│                                                             │
│  memmove(buffer, buffer+n, ...) → OVERLAP → dùng memmove │
└─────────────────────────────────────────────────────────────┘
```
