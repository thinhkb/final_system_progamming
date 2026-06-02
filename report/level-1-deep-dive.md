# Level 1 Deep Dive — System Programming Fundamentals

Bài viết này giải thích chi tiết 5 khái niệm nền tảng từ Level 1, mỗi khái niệm đều được map đến code cụ thể trong project.

---

## Mục lục

1. [File Descriptors](#1-file-descriptors)
2. [User Space vs Kernel Space](#2-user-space-vs-kernel-space)
3. [Buffering in C](#3-buffering-in-c)
4. [The errno Pattern](#4-the-errno-pattern)
5. [POSIX Model](#5-posix-model)

---

## 1. File Descriptors

### 1.1 Khái niệm

**File descriptor (fd)** là một số nguyên không âm mà kernel trả về khi một process mở một resource (file, socket, pipe, device...). Process dùng fd để thao tác với resource đó thay vì dùng tên.

```
fd = 0  → stdin  (standard input, bàn phím)
fd = 1  → stdout (standard output, màn hình)
fd = 2  → stderr (standard error, màn hình)
fd = 3  → opened next
fd = 4  → opened next
...
```

### 1.2 File Descriptor Table (per-process)

Mỗi process có một **file descriptor table** — một mảng trong user space:

```
Process A's FD Table (user space):
┌─────┬────────────────────────────────┐
│ fd  │ Kernel File Description Pointer │
├─────┼────────────────────────────────┤
│  0  │  → stdin (keyboard)           │
│  1  │  → stdout (screen)           │
│  2  │  → stderr (screen)           │
│  3  │  → /www/index.html           │
│  4  │  → TCP socket to client:8080  │
│  ... │  ...                           │
└─────┴────────────────────────────────┘
```

Mỗi entry trỏ đến một **kernel file description** (shared giữa tất cả process có cùng file/system). Kernel file description chứa:

- File offset (vị trí hiện tại trong file)
- Access mode (read/write)
- Flags (O_NONBLOCK, O_APPEND...)
- Pointer đến inode/filesystem structure

### 1.3 FD trong project này

Trong HTTP server, fd được dùng cho 3 loại resource:

```c
// src/server.c:478–511
int create_listening_socket(const server_config_t *config) {
    // Tạo socket → trả về fd mới (ví dụ fd=3)
    listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    // Nếu thành công: listen_fd ≥ 3

    // bind() → gắn socket vào IP:port
    bind(listen_fd, rp->ai_addr, rp->ai_addrlen);

    // listen() → biến thành passive socket
    listen(listen_fd, SOMAXCONN);

    return listen_fd;  // trả fd cho caller
}
```

```c
// src/server.c:557
// accept() tạo MỘT fd MỚI cho mỗi kết nối client
int client_fd = accept(server->listen_fd, NULL, NULL);
// Nếu 10 clients kết nối → có 10 client_fd khác nhau
// listen_fd (fd của listening socket) không đổi
```

```c
// src/log.c:26
// fopen() trả về FILE* — bên trong có fd
created->file = fopen(path, "a");
// fopen() gọi syscall open() bên trong → trả về fd ≥ 3
// FILE* đóng gói fd thành higher-level C stream
```

### 1.4 Đóng FD

```c
// src/server.c:475
close(client_fd);  // đóng kết nối client

// src/server.c:594, 604–605
if (server->listen_fd >= 0) {
    close(server->listen_fd);  // đóng listening socket khi shutdown
}

// src/log.c:47
fclose(log->file);  // fclose() gọi close() bên trong
```

**Quy tắc vàng:** Mỗi `open()`, `socket()`, `accept()`, `fopen()` đều tạo fd mới. Khi xong việc phải `close()` hoặc `fclose()`. Không đóng → **fd leak** → eventually run out of fd → server crash.

### 1.5 FD Limits

```bash
# Xem soft limit cho process hiện tại
ulimit -n          # thường là 1024 hoặc 4096

# Xem hard limit
ulimit -Hn

# Tăng tạm thời cho session
ulimit -n 65535
```

FD leak check:

```bash
# Đếm fd đang mở của process
lsof -p $(pgrep httpd) | wc -l

# Xem tất cả fd đang mở
ls /proc/$(pgrep httpd)/fd
```

### 1.6 fd ≥ 0 convention

```c
// src/server.c:520
server->listen_fd = -1;  // -1 = chưa mở (invalid fd)

// src/server.c:532–533
server->listen_fd = create_listening_socket(config);
if (server->listen_fd < 0) {  // lỗi
    // cleanup...
}
```

Convention trong project: fd ≥ 0 là valid, -1 là error sentinel. `socket()`, `open()`, `accept()` trả về -1 khi lỗi.

---

## 2. User Space vs Kernel Space

### 2.1 Hai vùng nhớ riêng biệt

```
┌─────────────────────────────────────────────────┐
│  User Space (Userspace)                         │
│                                                 │
│  • Code của process (text segment)              │
│  • Heap (malloc)                               │
│  • Stack (local variables)                      │
│  • Global data                                  │
│                                                 │
│  NGUYÊN TẮC: Không thể truy cập trực tiếp     │
│  kernel memory từ đây                           │
└─────────────────────────────────────────────────┘
                    ↑ syscalls (Boundary)
                    ↓ return
┌─────────────────────────────────────────────────┐
│  Kernel Space (Kernelspace)                     │
│                                                 │
│  • Process scheduler                            │
│  • File system drivers                          │
│  • Network stack (TCP/IP)                       │
│  • Device drivers                               │
│  • Memory manager                               │
│                                                 │
│  Privileged mode: có thể truy cập mọi thứ       │
│  trong máy (hardware registers, memory...)      │
└─────────────────────────────────────────────────┘
```

### 2.2 System Calls = Crossing the Boundary

**System call (syscall)** là cơ chế duy nhất để user space nói chuyện với kernel. Mỗi syscall:

1. Đặt arguments vào registers
2. Gọi software interrupt / `syscall` instruction
3. CPU chuyển sang kernel mode (privileged)
4. Kernel thực thi operation
5. Trả kết quả về user space
6. CPU quay lại user mode

```
User space                          Kernel space
   │                                     │
   │  recv(client_fd, buffer, 8192, 0)   │
   │ ──────────────────────────────────→ │
   │                                     │
   │  [Kernel nhận data từ network card]  │
   │  [Copy data vào buffer trong user    │
   │   space — đây là kernel→user copy]  │
   │                                     │
   │ ←─────────────────────────────────── │
   │  return số bytes received             │
```

### 2.3 Syscalls trong project

| Syscall            | Mô tả                     | User/Kernel                                          | File              |
| ------------------ | --------------------------- | ---------------------------------------------------- | ----------------- |
| `socket()`       | Tạo socket                 | Userspace gọi → Kernel tạo socket                 | `server.c`      |
| `bind()`         | Gắn IP:port                | Userspace gọi → Kernel gán addr                   | `server.c`      |
| `listen()`       | Biến thành passive socket | Userspace gọi → Kernel setup                       | `server.c`      |
| `accept()`       | Chấp nhận kết nối       | Userspace gọi → Kernel trả client socket          | `server.c`      |
| `recv()`         | Nhận data từ socket       | Userspace gọi → Kernel copy data                   | `server.c`      |
| `send()`         | Gửi data qua socket        | Userspace gọi → Kernel copy data to NIC            | `server.c`      |
| `close()`        | Đóng fd                   | Userspace gọi → Kernel giải phóng resource       | `server.c`      |
| `read()`         | Đọc từ file descriptor   | Userspace gọi → Kernel copy file data              | `files.c`       |
| `write()`        | Ghi vào fd                 | Userspace gọi → Kernel ghi data                    | `log.c`         |
| `fopen()`        | Mở file                    | **Library call (không phải syscall trực tiếp)** | **`log.c`**    |
| **`fread()`**   | **Đọc file buffered**    | **Library call → gọi `read()` bên trong**      | **`server.c`** |
| **`fprintf()`** | **Ghi formatted output**   | **Library call → gọi `write()` bên trong**     | `log.c`         |
| `stat()`         | Lấy file metadata          | Userspace gọi → Kernel trả inode info             | `files.c`       |
| `realpath()`     | Resolve symlinks            | Userspace gọi → Kernel/FS interaction              | `files.c`       |
| `opendir()`      | Mở directory               | Userspace gọi → Kernel trả DIR*                   | `files.c`       |
| `readdir()`      | Đọc directory entry       | Userspace gọi → Kernel trả entry                  | `files.c`       |
| `getaddrinfo()`  | Resolve hostname            | Userspace gọi → Kernel/DNS lookup                  | `server.c`      |
| `getpeername()`  | Lấy client IP              | Userspace gọi → Kernel trả peer addr              | `server.c`      |
| `localtime_r()`  | Convert timestamp           | Userspace only (no kernel call)                      | `log.c`         |

### 2.4 Library Calls vs System Calls

Đây là điểm hay bị nhầm lẫn nhất trong System Programming:

```
┌──────────────────────────────────────────────────────────┐
│  User Space                                               │
│                                                           │
│  ┌──────────────────────────────────────────────────┐   │
│  │  libc (glibc / macOS libSystem)                   │   │
│  │                                                   │   │
│  │  fopen() ──→ gọi syscall open() bên trong        │   │
│  │  fread() ──→ gọi syscall read() bên trong       │   │
│  │  fprintf() ──→ gọi syscall write() bên trong     │   │
│  │  malloc() ──→ gọi syscall brk()/mmap() bên trong │   │
│  │  localtime_r() ──→ pure C, no syscall              │   │
│  └──────────────────────────────────────────────────┘   │
│                                                           │
│  ┌──────────────────────────────────────────────────┐   │
│  │  User code                                         │   │
│  │                                                   │   │
│  │  send() ──→ TRỰC TIẾP syscall, không qua library │   │
│  │  recv() ──→ TRỰC TIẾP syscall, không qua library│   │
│  │  socket() ──→ TRỰC TIẾP syscall                   │   │
│  │  open() ──→ TRỰC TIẾP syscall                    │   │
│  └──────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────┘
```

**Trong project này:**

```c
// src/server.c — TRỰC TIẾP syscall, KHÔNG qua library:
ssize_t received = recv(client_fd, buffer + *buffered, READ_BUFFER_SIZE - *buffered, 0);
// recv() là syscall — trả về bytes nhận được hoặc -1

// src/log.c — gọi fprintf() (library call), bên trong fprintf gọi write() (syscall):
fprintf(log->file, "%s - - [%s] \"%s\" %d %zu\n", ...);
// fprintf là library function, bên trong nó gọi write() syscall

// src/server.c — fread() là library call, bên trong gọi read() syscall:
size_t got = fread(buffer, 1, want, file);
// fread() đọc từ buffer, khi buffer hết mới gọi read() syscall
```

### 2.5 Copying Data: The Key Distinction

Syscalls liên quan đến I/O đều cần **copy data** giữa user space và kernel space:

```c
// src/server.c:122
ssize_t received = recv(client_fd, buffer + *buffered, READ_BUFFER_SIZE - *buffered, 0);
//                          ↑
//                          │
// ĐÂY LÀ COPY: kernel → user space
// Kernel copy data từ TCP buffer (kernel space) vào buffer[] (user space)
```

```c
// src/server.c:324–336 — streaming file
while (remaining > 0) {
    size_t got = fread(buffer, 1, want, file);  // library: file → user buffer
    // fread bên trong gọi read(): kernel đọc từ disk → copy vào user buffer
    if (send_all(fd, buffer, got) != 0) ...     // syscall: user buffer → kernel → NIC
    // send() syscall: copy từ user buffer → kernel TCP buffer → network card
}
```

### 2.6 Tại sao phân biệt User/Kernel quan trọng?

1. **Security:** User code không thể truy cập kernel memory trực tiếp → không thể đọc RAM của kernel/OS
2. **Stability:** Nếu user code crash, kernel không crash → máy không treo
3. **Performance:** Mỗi syscall có overhead (context switch) → gọi ít syscall hơn = nhanh hơn

```c
// VÍ DỤ: Tại sao dùng large buffer?
// Bad: gọi recv() nhiều lần cho 1KB data
for (int i = 0; i < 1000; i++) {
    recv(fd, buf, 1, 0);  // 1000 syscalls!
}
// Good: đọc nhiều 1 lần
recv(fd, buf, 8192, 0);  // 1 syscall đọc nhiều bytes
```

---

## 3. Buffering in C

### 3.1 Ba mức độ buffering

Khi bạn gọi `fprintf()` hoặc `fread()`, C library không gọi syscall ngay lập tức. Nó dùng **buffers** để giảm số lượng syscalls:

```
┌─────────────────────────────────────────────────────────────┐
│  User Code                                                 │
│  fprintf(file, "log line\n");                              │
│  │                                                         │
│  │  libc copy "log line\n" vào FILE buffer (user space)  │
│  ▼                                                         │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  FILE struct:                                        │   │
│  │    char buffer[BUFSIZE];  ← đây là buffering       │   │
│  │    FILE *file;                                      │   │
│  │    int flags;  (_IONBF | _IOLBF | _IOFBF)         │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Buffer modes:                                             │
│  ┌─────────┬────────────────────────────────────────────┐ │
│  │ _IONBF  │ Unbuffered: gọi syscall MỖI LẦN write    │ │
│  ├─────────┼────────────────────────────────────────────┤ │
│  │ _IOLBF  │ Line-buffered: flush khi gặp '\n'        │ │
│  ├─────────┼────────────────────────────────────────────┤ │
│  │ _IOFBF  │ Fully buffered: flush khi buffer đầy     │ │
│  └─────────┴────────────────────────────────────────────┘ │
│                                                             │
│  Khi flush (hoặc buffer đầy):                             │
│  │  write() syscall: kernel ← user buffer                 │
│  ▼                                                         │
│  Kernel: copy vào page cache / network buffer              │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Default buffering trong project

```c
// src/log.c:26
created->file = fopen(path, "a");  // "a" = append mode

// fopen("a", ...) default: _IOFBF (fully buffered)
// Buffer thường là 4KB hoặc 8KB tùy platform
```

Điều này có nghĩa là: nếu server log 100 requests nhanh, có thể chỉ 1 `write()` syscall được gọi cho 100 dòng log.

### 3.3 Tại sao `fflush()` trong `log.c`?

```c
// src/log.c:64–68
pthread_mutex_lock(&log->mutex);
fprintf(log->file, "%s - - [%s] \"%s\" %d %zu\n", ...);
// fprintf chỉ copy vào buffer, CHƯA ghi xuống disk
fflush(log->file);  // ← bắt buộc flush để kernel nhận data
pthread_mutex_unlock(&log->mutex);
```

**Vấn đề nếu không fflush:**

```
Thread A: fprintf("127.0.0.1 - ...")
Thread B: fprintf("192.168.1.1 - ...")
Thread A: exit() ← process crash trước khi fflush
─────────────────────────────────
// Kết quả: dòng của Thread A bị MẤT trong buffer
```

**Giải pháp:** `fflush()` nằm TRONG mutex đảm bảo mỗi dòng được flush TRƯỚC KHI unlock.

### 3.4 So sánh: C buffered I/O vs POSIX recv/send

```c
// C buffered I/O (FILE* interface):
fprintf(log->file, "...");   // copy vào buffer
fflush(log->file);            // syscall write()
// ─────────────────────────────────
// Đặc điểm:
// • Higher-level API (formatted output)
// • Tự động buffering, tự động resize
// • fmt specifiers (%s, %d, %zu, %zu)
// • Gọi write() syscall khi buffer đầy hoặc flush

// POSIX raw socket I/O (fd interface):
send(fd, data, len, 0);   // syscall send() TRỰC TIẾP
recv(fd, buffer, size, 0); // syscall recv() TRỰC TIẾP
// ─────────────────────────────────
// Đặc điểm:
// • Không buffering
// • Mỗi call = 1 syscall
// • Không formatted output
// • Kiểm soát socket options qua setsockopt()
```

**Trong project:**

- `src/log.c` dùng `fprintf()` + `fflush()` — C buffered I/O vì cần formatted output
- `src/server.c` dùng `recv()` + `send()` — POSIX raw vì kiểm soát socket cấp thấp
- `src/server.c` dùng `fread()` + `fseeko()` — C buffered vì đọc file theo chunks

### 3.5 recv() buffer size

```c
// src/server.c:31
#define READ_BUFFER_SIZE 8192  // 8KB buffer

// src/server.c:122
ssize_t received = recv(client_fd, buffer + *buffered,
                        READ_BUFFER_SIZE - *buffered, 0);
```

Tại sao 8KB?

- TCP/IP default MTU là ~1500 bytes → mỗi network packet nhỏ hơn buffer
- 8KB là phổ biến: balance giữa memory usage và syscall frequency
- HTTP request thường nhỏ (< 8KB headers + small body)

### 3.6 File streaming với fseeko

```c
// src/server.c:278
if (fseeko(file, (off_t)range.start, SEEK_SET) != 0) {
    fclose(file);
    return send_simple_response(fd, request, 500, ...);
}
```

`fseeko()` thay đổi **file position indicator** trong `FILE*` struct. Mỗi `fread()` tiếp theo đọc từ vị trí mới.

```
FILE struct bên trong:
┌──────────────────────────────────────┐
│  char buffer[8192];                  │
│  size_t buffer_pos;  ← vị trí trong buffer │
│  off_t file_pos;    ← vị trí trong file │
│  FILE *underlying_fd;                │
└──────────────────────────────────────┘

fseeko(file, 100, SEEK_SET):
  → file_pos = 100

fread(buffer, 1, 4096, file):
  → read syscall: đọc bytes 100–4195
  → buffer chứa bytes đã đọc
  → file_pos = 4196
```

---

## 4. The errno Pattern

### 4.1 errno là gì?

`errno` là một **global variable** trong C library được set bởi **hầu hết các syscall** khi có lỗi xảy ra. Nó chứa mã lỗi (số nguyên) mà bạn tra để hiểu **TẠI SAO** syscall thất bại.

```c
// src/files.c:448
if (realpath(doc_root, root_real) == NULL) {
    // realpath trả về NULL khi lỗi
    // errno được set bởi realpath()
    return FILE_RESULT_ERROR;
}
```

### 4.2 errno không bị clear khi thành công

**ĐÂY LÀ PITFALL LỚN NHẤT:**

```c
// SAI:
int fd = open("file.txt", O_RDONLY);
if (fd < 0) {
    printf("Error: %s\n", strerror(errno));  // OK nếu < 0
}
printf("File opened: %s\n", strerror(errno));  // SAI! errno có thể còn từ lần gọi trước

// ĐÚNG:
int fd = open("file.txt", O_RDONLY);
if (fd < 0) {
    printf("Error: %s\n", strerror(errno));
}
// Khi fd ≥ 0, không được dùng errno vì nó không defined cho thành công
```

### 4.3 errno values được dùng trong project

#### 4.3.1 EINTR — Syscall bị interrupt bởi signal

```c
// src/server.c:123–128
ssize_t received = recv(client_fd, buffer + *buffered, READ_BUFFER_SIZE - *buffered, 0);
if (received < 0) {
    if (errno == EINTR) {
        continue;  // Signal xảy ra giữa chừng → retry
    }
    return -1;  // Lỗi thật sự
}
```

**Giải thích:** Khi process đang blocked trong `recv()` (đợi data từ network), nếu signal handler được gọi (ví dụ: SIGINT từ Ctrl+C), syscall bị interrupt và trả về -1 với `errno = EINTR`. Đây **không phải lỗi** — cần retry.

```c
// src/server.c:561–566
if (client_fd < 0) {
    if (errno == EINTR) {
        continue;  // accept() bị interrupt → retry
    }
    if (server->should_stop) {
        break;     // Server đang shutdown
    }
    continue;       // accept() lỗi khác → continue
}
```

#### 4.3.2 ENOENT — File or directory does not exist

```c
// src/files.c:452–454
if (realpath(joined, target_real) == NULL) {
    if (errno == ENOENT || errno == ENOTDIR) {
        return FILE_RESULT_NOT_FOUND;  // File không tồn tại
    }
    return FILE_RESULT_ERROR;  // Lỗi khác (permission, I/O...)
}
```

#### 4.3.3 ENOTDIR — Not a directory component in path

```c
// src/files.c:453
if (errno == ENOENT || errno == ENOTDIR) {
    return FILE_RESULT_NOT_FOUND;
}
// Ví dụ: /dir/file/extra → "extra" không phải directory
// Nhưng cũng có thể là ENOENT (không tồn tại)
```

### 4.4 Full errno table cho các syscall trong project

| errno       | Ý nghĩa                           | Syscall                                          | Xử lý trong code                                    |
| ----------- | ----------------------------------- | ------------------------------------------------ | ----------------------------------------------------- |
| `ENOENT`  | Không tồn tại                    | `realpath()`, `stat()`, `open()`           | Return `FILE_RESULT_NOT_FOUND`                      |
| `ENOTDIR` | Component không phải directory    | `realpath()`                                   | Gộp với `ENOENT` → `NOT_FOUND`                 |
| `EINTR`   | Bị interrupt bởi signal           | `accept()`, `recv()`, `send()`, `open()` | Retry (`continue`)                                  |
| `EAGAIN`  | Non-blocking: operation would block | `recv()` (non-blocking mode)                   | Retry hoặc skip                                      |
| `EBADF`   | Bad file descriptor                 | `close()`, `read()`                          | Không xử lý đặc biệt                            |
| `EFAULT`  | Bad address (invalid pointer)       | `realpath()`                                   | Không xảy ra nếu code đúng                       |
| `ENOMEM`  | Out of memory                       | `malloc()`, `calloc()`                       | Process bị kill by OOM killer                        |
| `EPERM`   | Permission denied                   | `stat()`, `open()`                           | Trả về `FILE_RESULT_FORBIDDEN` (cần thêm check) |
| `EMFILE`  | Too many open files                 | `open()`, `socket()`, `accept()`           | Server không handle (cần raise limit)               |

### 4.5 strerror() — Human-readable error message

```c
#include <string.h>

// src/log.c không dùng strerror (log không cần)
// Nhưng khi debug, bạn có thể dùng:

if (realpath(path, resolved) == NULL) {
    fprintf(stderr, "realpath failed: %s\n", strerror(errno));
    // Output: realpath failed: No such file or directory
}
```

### 4.6 perror() — Convenient alternative

```c
// In ra: "realpath: No such file or directory"
if (realpath(path, resolved) == NULL) {
    perror("realpath");
}
```

### 4.7 Thread Safety của errno

**Vấn đề:** `errno` là một **global variable**. Nếu hai threads gọi syscalls cùng lúc, thread A set errno, thread B set errno, kết quả là giá trị cuối cùng ghi đè.

**Giải pháp trên macOS/FreeBSD:** Mỗi thread có **errno riêng** (thread-local storage). Trên Linux, errno cũng là thread-local.

**Tuy nhiên**, đây vẫn là pattern tốt để check errno NGAY SAU syscall trước khi gọi bất kỳ function nào khác:**

```c
// ĐÚNG: check ngay sau syscall
int fd = open(path, O_RDONLY);
if (fd < 0) {
    // errno được set ngay tại đây
    if (errno == ENOENT) return NOT_FOUND;
    return ERROR;
}

// SAI: có thể bị overwrite bởi function khác
int fd = open(path, O_RDONLY);
log_message("Opening file");  // nếu log_message gọi syscall, errno có thể bị thay đổi
if (fd < 0) {
    // errno có thể không còn là giá trị từ open() nữa
    if (errno == ENOENT) ...  // BUG!
}
```

### 4.8 errno trong project — Tổng hợp

```c
// src/server.c:37–41
// EINTR handling trong send_all:
ssize_t n = send(fd, data + sent, length - sent, 0);
if (n < 0) {
    if (errno == EINTR) {
        continue;  // Retry
    }
    return -1;
}

// src/files.c:452–456
// errno từ realpath():
if (realpath(joined, target_real) == NULL) {
    if (errno == ENOENT || errno == ENOTDIR) {
        return FILE_RESULT_NOT_FOUND;  // File not found
    }
    return FILE_RESULT_ERROR;  // Other errors
}

// src/server.c:561
// errno từ accept():
if (client_fd < 0) {
    if (errno == EINTR) {
        continue;
    }
    ...
}
```

---

## 5. POSIX Model

### 5.1 POSIX là gì?

**POSIX (Portable Operating System Interface)** là một **chuẩn** do IEEE định nghĩa, mô tả interface giữa user programs và OS. Mục tiêu: code viết theo POSIX chạy được trên nhiều hệ điều hành (Linux, macOS, BSD, Unix...).

```
┌────────────────────────────────────────────────────────────┐
│  Application Code (POSIX compliant)                        │
│  socket(), recv(), pthread_create(), open(), stat()       │
└────────────────────────────────────────────────────────────┘
          │                        │
          ▼                        ▼
    ┌──────────┐           ┌──────────────┐
    │  Linux   │           │    macOS     │
    │ (glibc)  │           │ (libSystem)  │
    └──────────┘           └──────────────┘
```

### 5.2 Build flags trong project

```makefile
# Makefile:2
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic \
          -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
          -Iinclude
```

```c
// Giải thích từng flag:

// _POSIX_C_SOURCE=200809L
//
// Khai báo trước compile: "Tôi muốn dùng POSIX.1-2008 functions"
// Nếu không khai báo, compiler có thể không expose một số POSIX functions
//
// Các giá trị phổ biến:
//   200112L  → POSIX.1-2001
//   200809L  → POSIX.1-2008 (dùng trong project)
//   201712L  → POSIX.1-2017

// _XOPEN_SOURCE=700
//
// Mở rộng từ XSI (X/Open System Interface)
// 700 = SUSv4 (Single UNIX Specification Version 4)
// Cần cho: realpath(), nftw(), glob(), wordexp()
```

**Nếu không có flags này:**

```c
// Không có -D_POSIX_C_SOURCE=200809L:
// recv() có thể không được declare → compiler warning/error
// pthread_create() có thể không nhìn thấy

// Không có -D_XOPEN_SOURCE=700:
// realpath() có thể không được declare
```

### 5.3 Các POSIX components được dùng trong project

#### 5.3.1 POSIX Sockets

```c
// src/server.c
#include <sys/socket.h>   // POSIX socket API
#include <sys/types.h>    // data types
#include <netdb.h>        // getaddrinfo()

// Các functions:
socket(AF_INET, SOCK_STREAM, 0)  // POSIX
bind(listen_fd, addr, addrlen)   // POSIX
listen(listen_fd, SOMAXCONN)      // POSIX
accept(listen_fd, addr, addrlen)  // POSIX
recv(sockfd, buf, len, flags)    // POSIX
send(sockfd, buf, len, flags)    // POSIX
shutdown(sockfd, how)            // POSIX (not used)
close(sockfd)                    // POSIX
```

#### 5.3.2 POSIX Threads (pthreads)

```c
// src/thread_pool.c
#include <pthread.h>

pthread_create(&tid, NULL, worker_main, args);    // tạo thread
pthread_join(tid, NULL);                         // đợi thread kết thúc
pthread_mutex_init(&mutex, NULL);                // khởi tạo mutex
pthread_mutex_lock(&mutex);                     // acquire lock
pthread_mutex_unlock(&mutex);                   // release lock
pthread_cond_init(&cond, NULL);                  // khởi tạo condition variable
pthread_cond_wait(&cond, &mutex);              // blocking wait
pthread_cond_signal(&cond);                     // wake one
pthread_cond_broadcast(&cond);                  // wake all
pthread_mutex_destroy(&mutex);                  // cleanup
pthread_cond_destroy(&cond);                    // cleanup
```

#### 5.3.3 POSIX File Operations

```c
// src/files.c
#include <sys/stat.h>    // stat(), S_ISDIR(), S_ISREG()
#include <dirent.h>      // opendir(), readdir(), closedir()

stat(path, &st)                    // get file metadata
S_ISDIR(st.st_mode)               // is directory?
S_ISREG(st.st_mode)              // is regular file?
realpath(path, resolved)          // resolve to canonical path
opendir(path)                     // open directory
readdir(dir)                      // read next entry
closedir(dir)                    // close directory
```

#### 5.3.4 POSIX Time

```c
// src/log.c
#include <time.h>

time_t now = time(NULL);                  // current timestamp
localtime_r(&now, &tm_buf);              // convert to local time (thread-safe)
strftime(buf, size, format, &tm_buf);   // format time string

// Format string cho Common Log Format:
// "%d/%b/%Y:%H:%M:%S %z"
// 10/Oct/2026:13:55:36 +0000
```

### 5.4 Tại sao dùng POSIX thay vì platform-specific?

```c
// POSIX (chạy trên Linux + macOS + BSD):
int fd = socket(AF_INET, SOCK_STREAM, 0);
struct stat st;
stat(path, &st);
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Windows-specific (KHÔNG dùng trong project):
SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);  // SOCKET, không phải int
HANDLE mutex = CreateMutex(NULL, FALSE, NULL); // KHÁC hoàn toàn
```

**Benefit của POSIX:** Code viết 1 lần, compile trên Linux và macOS đều works.

### 5.5 C11 vs POSIX

Project dùng `-std=c11` (C standard) nhưng kết hợp với POSIX extensions:

```c
// C11: threading.h (stdthreads) — KHÔNG dùng
// pthread.h là POSIX, không phải C standard
#include <pthread.h>    // POSIX (không phải C11)

// C11 threads (không dùng):
// #include <threads.h>
// thrd_create(), mtx_init(), cnd_wait() — khác hoàn toàn
```

**Quyết định:** Project dùng `pthread.h` (POSIX) vì:

- `pthread` là de facto standard trên Unix-like systems
- C11 `<threads.h>` implementation trên macOS/Linux không đầy đủ
- Production servers (nginx, apache) đều dùng pthread

### 5.6 POSIX compliance levels

| Standard     | Year | Key Features                                                         | Flag                                         |
| ------------ | ---- | -------------------------------------------------------------------- | -------------------------------------------- |
| POSIX.1      | 1988 | Original:`open()`, `read()`, `write()`, `fork()`, `exec()` | `_POSIX_SOURCE`                            |
| POSIX.1b     | 1993 | Realtime extensions: semaphores, memory-mapped files, scheduling     | `_POSIX_C_SOURCE=199309L`                  |
| POSIX.1c     | 1995 | Threads (pthread)                                                    | `_POSIX_C_SOURCE=199506L`                  |
| POSIX.1-2001 | 2001 | Updated: better pthread, spin locks                                  | `_POSIX_C_SOURCE=200112L`                  |
| POSIX.1-2008 | 2008 | Current: improved async I/O, advisory information                    | `_POSIX_C_SOURCE=200809L` ← project dùng |
| POSIX.1-2017 | 2017 | Latest: improved file sharing, barrier wait                          | `_POSIX_C_SOURCE=201712L`                  |

### 5.7 POSIX và macOS

macOS tuân thủ POSIX nhưng có một số khác biệt:

| Aspect           | Linux                                  | macOS                            |
| ---------------- | -------------------------------------- | -------------------------------- |
| Thread priority  | `pthread_setschedparam()` works well | Limited                          |
| `accept4()`    | Có                                    | Không (phải dùng `fcntl()`) |
| `sendfile()`   | Linux-specific                         | Không có                       |
| `SO_REUSEPORT` | Có                                    | Có (từ macOS 10.14)            |
| `/dev/poll`    | Có                                    | Không (dùng `kqueue`)        |
| `eventfd()`    | Có                                    | Không                           |

Project dùng `SO_REUSEADDR` (có trên cả hai), không dùng Linux-specific features → portable.

---

## Tổng kết: Level 1 Quick Reference

```
┌─────────────────────────────────────────────────────────────────┐
│ FILE DESCRIPTORS                                                │
│                                                                 │
│  fd = 0,1,2 (stdin/stdout/stderr)                              │
│  fd ≥ 3 (socket, file, device)                                 │
│  fd < 0 = error                                                │
│  Mỗi open/socket/accept → fd mới                              │
│  Mỗi close/fclose → giải phóng fd                             │
│  fd leak → server crash khi hết fd                            │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ USER SPACE vs KERNEL SPACE                                      │
│                                                                 │
│  User space: code + heap + stack của process                   │
│  Kernel space: drivers + FS + network stack + scheduler        │
│  Boundary: SYSCALLS (recv, send, read, write, open...)        │
│  Library calls (fprintf, fread) → gọi syscalls bên trong      │
│ recv/send → trực tiếp syscall, không buffering                │
│  fprintf/fread → buffered, gọi syscall khi cần                │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ BUFFERING                                                       │
│                                                                 │
│  _IONBF = unbuffered (mỗi syscall)                            │
│  _IOLBF = line-buffered (flush khi '\n')                      │
│  _IOFBF = fully-buffered (flush khi buffer đầy) ← default    │
│  fflush() = ép flush ngay lập tức                             │
│  fflush() TRONG mutex = đảm bảo mỗi log line đến kernel      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ ERRNO PATTERN                                                   │
│                                                                 │
│  errno = global variable, được set khi SYSCALL thất bại       │
│  errno không bị clear khi syscall thành công                   │
│  Check errno NGAY SAU syscall, trước khi gọi function khác      │
│  EINTR = syscall bị interrupt bởi signal → RETRY               │
│  ENOENT = file/dir không tồn tại                              │
│  ENOTDIR = component không phải directory                      │
│  strerror(errno) → human-readable message                      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ POSIX                                                           │
│                                                                 │
│  POSIX = chuẩn interface giữa app và OS                        │
│  POSIX.1-2008 = phiên bản dùng trong project (_POSIX_C_SOURCE) │
│  XSI = mở rộng POSIX cho Unix (realpath, glob...)             │
│  pthread = POSIX threads (create, join, mutex, condvar)        │
│  -D_POSIX_C_SOURCE=200809L = khai báo muốn dùng POSIX.1-2008  │
│  -D_XOPEN_SOURCE=700 = khai báo muốn dùng XSI/SUSv4          │
│  Project dùng POSIX sockets + POSIX threads + POSIX file ops   │
└─────────────────────────────────────────────────────────────────┘
```
