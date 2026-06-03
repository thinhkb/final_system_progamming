# Level 2 Deep Dive — Networking (TCP/IP Sockets)

Level 2 đánh dấu thời điểm chúng ta rời tầng application (nơi HTTP headers được parse) và đi xuống tầng transport — nơi các byte được truyền qua mạng TCP/IP thực sự. Đây là tầng "ngầm" mà mọi HTTP server chạy trên đó.

---

## Mục lục

1. [TCP 3-way Handshake — Bức tranh toàn cảnh](#1-tcp-3-way-handshake)
2. [Socket Lifecycle — Từ `socket()` đến `close()`](#2-socket-lifecycle)
3. [`socket()` — Tạo endpoint](#3-socket--tạo-endpoint)
4. [`getaddrinfo()` — Phân giải địa chỉ](#4-getaddrinfo--phân-giải-địa-chỉ)
5. [`bind()` — Gắn địa chỉ vào socket](#5-bind--gắn-địa-chỉ-vào-socket)
6. [`listen()` — Chuyển sang passive mode](#6-listen--chuyển-sang-passive-mode)
7. [`accept()` — Chấp nhận kết nối](#7-accept--chấp-nhận-kết-nối)
8. [`send()` / `recv()` — Truyền nhận dữ liệu](#8-send--recv--truyền-nhận-dữ-liệu)
9. [`SO_REUSEADDR` — Tái sử dụng cổng](#9-so_reuseaddr--tái-sử-dụng-cổng)
10. [Blocking vs Non-blocking I/O](#10-blocking-vs-non-blocking-io)
11. [TCP State Machine &amp; TIME_WAIT](#11-tcp-state-machine--time_wait)
12. [Đi sâu vào TCP Buffer](#12-đi-sâu-vào-tcp-buffer)
13. [Bigger Picture: HTTP trên TCP](#13-bigger-picture-http-trên-tcp)

---

## 1. TCP 3-way Handshake — Bức tranh toàn cảnh

### 1.1 Tại sao cần handshake?

TCP là **reliable** protocol — nó đảm bảo mọi byte gửi đi sẽ đến đích, theo đúng thứ tự, không bị lỗi. Để làm được điều này, trước khi truyền dữ liệu, client và server phải **đồng ý** về trạng thái kết nối:

```
Client                              Server
   │                                    │
   │ ──────── SYN (seq=x) ────────────→ │  Client: "Tôi muốn sync!"
   │   Bandwidth: SYN packet (40-60 bytes)    │
   │                                    │
   │ ←────── SYN-ACK (seq=y, ack=x+1) ───── │  Server: "OK, tôi thấy, và đây là seq của tôi"
   │   Bandwidth: SYN-ACK packet              │
   │                                    │
   │ ──────── ACK (ack=y+1) ─────────────→ │  Client: "Tôi thấy, bắt đầu truyền!"
   │                                    │
   │ ═══════ Connection Established ════════ │  ← Bây giờ mới gửi HTTP request được
   │                                    │
```

### 1.2 Chi tiết từng bước

**Bước 1: Client gửi SYN**

```
TCP Header:
┌──────────────┬──────────────┬──────────────┬──────────────┐
│  Source Port │ Dest Port    │ Sequence Num │    Flags     │
│    49152     │    8080      │    1000     │   SYN=1     │
└──────────────┴──────────────┴──────────────┴──────────────┘
                        ↑
                        │
                  Client chọn một
                  sequence number
                  ngẫu nhiên (ISN)
```

**Bước 2: Server nhận SYN, gửi SYN-ACK**

```
TCP Header:
┌──────────────┬──────────────┬──────────────┬──────────────┐
│  Source Port │ Dest Port    │ Sequence Num │    Flags     │
│    8080      │   49152     │    2000     │ SYN=1, ACK=1│
├──────────────┴──────────────┴──────────────┴──────────────┤
│              Acknowledgment: 1001 (= x + 1)               │
└──────────────────────────────────────────────────────────┘
                        ↑
                        │
                  Server chọn sequence
                  number riêng (ISN=2000)
                  ACK=1001 nghĩa là:
                  "Tôi đã nhận byte 1000,
                   mong nhận byte tiếp theo"
```

**Bước 3: Client gửi ACK**

```
TCP Header:
┌──────────────┬──────────────┬──────────────┬──────────────┐
│  Source Port │ Dest Port    │ Sequence Num │    Flags     │
│   49152      │    8080      │    1001     │    ACK=1    │
├──────────────┴──────────────┴──────────────┴──────────────┤
│              Acknowledgment: 2001 (= y + 1)               │
└──────────────────────────────────────────────────────────┘
```

### 1.3 Chi phí của handshake

```
Tổng chi phí trước khi gửi byte HTTP đầu tiên:
1 RTT (Round Trip Time) = thời gian client → server → client

Ví dụ:
- localhost:    ~0.1ms  → overhead thấp
- within LAN:  ~1ms    → overhead thấp
- US→Europe:   ~80ms   → OVERHEAD RẤT LỚN (80ms chờ handshake)
- Satellite:   ~600ms  → không dùng TCP được
```

**Tại sao HTTP Keep-Alive quan trọng?** Nếu mỗi request đều phải chờ 1 RTT để handshake, hiệu năng sẽ rất thấp. Keep-Alive giữ kết nối mở sau request đầu tiên → các request sau không cần handshake lại.

### 1.4 TCP teardown (4-way termination)

```
Client                              Server
   │                                    │
   │ ──────── FIN (seq=x) ────────────→ │  Client: "Tôi xong"
   │ ←─────── ACK (ack=x+1) ─────────── │  Server: "OK"
   │     (Client vào FIN_WAIT_2)        │
   │                                    │
   │ ←────── FIN (seq=y) ─────────────── │  Server: "Tôi cũng xong"
   │ ──────── ACK (ack=y+1) ────────────→ │  Client: "OK"
   │    (Client đợi 2*MSL rồi close)    │
   │                                    │
```

---

## 2. Socket Lifecycle — Từ `socket()` đến `close()`

### 2.1 Tổng quan 7 bước

```
┌─────────────────────────────────────────────────────────────────┐
│                    SOCKET LIFECYCLE                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. socket()        Tạo endpoint, nhận fd                    │
│           ↓                                                     │
│  2. getaddrinfo()  Lấy địa chỉ IP:port dạng struct          │
│           ↓                                                     │
│  3. setsockopt()    Cấu hình options (SO_REUSEADDR)            │
│           ↓                                                     │
│  4. bind()          Gắn IP:port vào socket                    │
│           ↓                                                     │
│  5. listen()        Chuyển thành passive socket                │
│           ↓                                                     │
│  6. accept() ← ──── [CLIENT: connect()]                        │
│           │        Nhận client_fd mới                           │
│           │                                                     │
│  7. send/recv()     Trao đổi dữ liệu                          │
│           ↓                                                     │
│  8. close()         Đóng socket                                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Trong project: Server side vs Client side

```c
// src/server.c:478–511 — SERVER SIDE
int create_listening_socket(const server_config_t *config) {
    struct addrinfo hints, *result, *rp;
    int listen_fd = -1;

    // 1. socket() — tạo endpoint
    listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

    // 2. getaddrinfo() — lấy địa chỉ
    getaddrinfo(config->host, port_text, &hints, &result);

    // 3. setsockopt() — SO_REUSEADDR
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 4. bind() — gắn IP:port
    bind(listen_fd, rp->ai_addr, rp->ai_addrlen);

    // 5. listen() — passive mode
    listen(listen_fd, SOMAXCONN);

    return listen_fd;
}
```

---

## 3. `socket()` — Tạo endpoint

### 3.1 Khái niệm

`socket()` tạo một **communication endpoint** — một điểm cuối mà dữ liệu có thể đi vào và ra. Kernel cấp phát một cấu trúc `struct socket` trong kernel space và trả về một File Descriptor (fd) cho user space.

```
User space                                          Kernel space
   │                                                   │
   │  listen_fd = socket(AF_INET, SOCK_STREAM, 0)    │
   │ ─────────────────────────────────────────────────→ │
   │                                                   │
   │  Kernel tạo:                                     │
   │  ┌────────────────────────────────────────────┐  │
   │  │ struct socket {                             │  │
   │  │   struct sock *sk;     ← TCP control block  │  │
   │  │   struct file *file;   ← fd table entry    │  │
   │  │   unsigned short type;  ← SOCK_STREAM      │  │
   │  │   unsigned long flags;                     │  │
   │  │   wait_queue_head_t wait;                  │  │
   │  │   ...                                     │  │
   │  │ }                                         │  │
   │  └────────────────────────────────────────────┘  │
   │                                                   │
   │  ←─── return fd = 3                             │
   │                                                   │
```

### 3.2 Tham số trong `socket()`

```c
// src/server.c:497
listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
```

| Tham số               | Giá trị       | Ý nghĩa                             |
| ---------------------- | --------------- | ------------------------------------- |
| `domain` (AF_INET)   | `AF_INET`     | IPv4                                  |
|                        | `AF_INET6`    | IPv6                                  |
|                        | `AF_UNIX`     | Unix domain socket (local)            |
| `type` (SOCK_STREAM) | `SOCK_STREAM` | TCP — oriented byte stream, reliable |
|                        | `SOCK_DGRAM`  | UDP — datagram, unreliable           |
|                        | `SOCK_RAW`    | Raw socket — bypass TCP/UDP          |
| `protocol`           | `0`           | Auto-select (TCP for SOCK_STREAM)     |
|                        | `IPPROTO_TCP` | Explicit TCP                          |
|                        | `IPPROTO_UDP` | Explicit UDP                          |

**Tại sao dùng `SOCK_STREAM`?** Vì HTTP cần reliable, ordered byte stream. `SOCK_DGRAM` (UDP) không đảm bảo thứ tự hoặc không mất gói — không phù hợp cho HTTP.

### 3.3 Điều gì xảy ra trong kernel?

Khi `socket()` được gọi, kernel:

1. Cấp phát một `struct socket` trong kernel heap
2. Gắn nó vào fd table entry của process (fd = số nguyên ≥ 3)
3. Khởi tạo `struct sock` (TCP control block) với trạng thái `CLOSED`
4. Trả fd về user space

---

## 4. `getaddrinfo()` — Phân giải địa chỉ

### 4.1 Vấn đề với cách cũ

```c
// Cách CŨ (deprecated, không dùng trong project):
struct hostent *host = gethostbyname("localhost");  // DNS lookup
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_port = htons(8080);
addr.sin_addr.s_addr = *(uint32_t*)host->h_addr_list[0];
bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
```

**Vấn đề:**

- `gethostbyname()` không hỗ trợ IPv6
- `struct sockaddr_in` chỉ dùng cho IPv4
- Phải tự xử lý nhiều edge cases
- Thread-unsafe (sử dụng static buffers)

### 4.2 Cách hiện đại: `getaddrinfo()`

```c
// src/server.c:486–494
memset(&hints, 0, sizeof(hints));
hints.ai_family = AF_UNSPEC;   // IPv4 và IPv6 đều được
hints.ai_socktype = SOCK_STREAM;  // TCP
hints.ai_flags = AI_PASSIVE;   // Dùng cho bind() — server socket

// Lấp đầy linked list 'result'
getaddrinfo(config->host, port_text, &hints, &result);

// Duyệt qua tất cả kết quả (có thể có nhiều: IPv4 + IPv6)
for (rp = result; rp != NULL; rp = rp->ai_next) {
    listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
        break;  // Thành công, dừng lại
    }
    close(listen_fd);  // Thử cái tiếp theo
}
freeaddrinfo(result);  // Giải phóng linked list
```

### 4.3 `struct addrinfo`

```
result → ai_next → ai_next → NULL
   │
   ├── ai_family:   AF_INET (IPv4)
   ├── ai_socktype: SOCK_STREAM (TCP)
   ├── ai_protocol: IPPROTO_TCP
   ├── ai_addrlen:  16 bytes
   ├── ai_addr:     → struct sockaddr_in {
   │                    sin_family: AF_INET
   │                    sin_port:   8080 (network byte order)
   │                    sin_addr:   0.0.0.0 (INADDR_ANY)
   │                }
   │
```

### 4.4 `AI_PASSIVE` flag

```c
hints.ai_flags = AI_PASSIVE;
```

`AI_PASSIVE` nói cho `getaddrinfo()` rằng địa chỉ này sẽ dùng cho `bind()` (server), không phải `connect()` (client).

- Với `AI_PASSIVE` + `host = NULL` → trả về `INADDR_ANY` (0.0.0.0) → bind vào tất cả interfaces
- Nếu không có `AI_PASSIVE` + `host = NULL` → trả về `localhost` (127.0.0.1)

### 4.5 Network Byte Order

```c
// src/server.c:490
snprintf(port_text, sizeof(port_text), "%d", config->port);
// port_text = "8080"
```

`getaddrinfo()` trả về `struct sockaddr` với port đã ở **network byte order** (big-endian). Trên máy little-endian (x86_64, ARM), `htons(8080)` chuyển 8080 từ host order sang network order.

```c
// Nếu muốn dùng trực tiếp:
uint16_t port_net = htons(8080);  // host-to-network-short

// Kiểm tra byte order:
printf("%04x\n", htons(0x1234));
// Output: 0x3412 (little-endian) → byte 0x34 đến trước
// Output: 0x1234 (big-endian)   → giữ nguyên
```

---

## 5. `bind()` — Gắn địa chỉ vào socket

### 5.1 Khái niệm

`bind()` gắn socket vào một **IP address + port cụ thể** trên máy. Nếu không bind, kernel sẽ tự động assign một port ngẫu nhiên (thường dùng cho client).

```
Server machine có 3 network interfaces:
┌─────────────────────────────────────────────────────────┐
│  lo0 (loopback):  127.0.0.1                            │
│  en0 (Wi-Fi):     192.168.1.100                        │
│  en1 (Ethernet):  10.0.0.5                             │
└─────────────────────────────────────────────────────────┘

bind(listen_fd, 0.0.0.0:8080)  → Chấp nhận từ TẤT CẢ interfaces
bind(listen_fd, 127.0.0.1:8080) → Chỉ loopback
bind(listen_fd, 192.168.1.100:8080) → Chỉ Wi-Fi
```

### 5.2 `struct sockaddr` — Generic socket address

```c
// addrinfo trả về struct sockaddr, nhưng mỗi protocol có struct riêng:

// IPv4:
struct sockaddr_in {
    sa_family_t sin_family;    // AF_INET
    in_port_t   sin_port;     // Port (network byte order)
    struct in_addr sin_addr;   // IP address
    char        sin_zero[8];  // Padding → đủ 16 bytes
};

// IPv6:
struct sockaddr_in6 {
    sa_family_t sin6_family;    // AF_INET6
    in_port_t   sin6_port;      // Port
    uint32_t    sin6_flowinfo;  // Flow label
    struct in6_addr sin6_addr;  // 16-byte IPv6 address
    uint32_t    sin6_scope_id;  // Scope ID
};
```

`getaddrinfo()` populate đúng struct dựa trên `ai_family`.

### 5.3 Port 0–1023 là privileged ports

```bash
# Trên Unix:
# Port 0–1023 = "well-known ports"
# Cần root/sudo mới bind() được

$ sudo ./httpd -p 80 -r www
# OK: root có thể bind port 80

$ ./httpd -p 8080 -r www
# OK: port > 1024 không cần privileged

$ ./httpd -p 443 -r www
# FAIL: EACCES (permission denied) — port 443 là privileged
```

**Giải pháp trong production:** Chạy server với root, sau đó `setuid()` xuống user thường:

```c
// Không có trong project này, nhưng production server thường làm:
if (getuid() == 0) {
    // Bind privileged port với root
    bind(listen_fd, ...);
    // Drop privileges
    setgid(65534);  // nobody
    setuid(65534);
}
```

---

## 6. `listen()` — Chuyển sang passive mode

### 6.1 Từ Active sang Passive socket

```c
// src/server.c:502
if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0 &&
    listen(listen_fd, SOMAXCONN) == 0) {
    break;
}
```

**Trước `listen()`:** Socket ở trạng thái `CLOSED`. Có thể gọi `connect()` (client) hoặc `bind()`.

**Sau `listen()`:** Socket chuyển sang `LISTEN`. Từ giờ chỉ có thể `accept()`.

### 6.2 Backlog — Hàng đợi kernel

```c
listen(listen_fd, SOMAXCONN);
```

**Backlog** = số lượng kết nối **đã handshake xong** (tức đã nhận SYN-ACK) đang chờ ứng dụng gọi `accept()`.

```
                            kernel
  Client ─── SYN ──────→ ┌──────────────────────────────────┐
                         │  ┌──────────────────────────────┐ │
                         │  │    Accept Backlog (LISTEN)   │ │
  Client ←── SYN-ACK ────┤  │  ┌────┬────┬────┬────┬────┐ │ │
                         │  │  │fd1 │fd2 │fd3 │fd4 │ .. │ │ │
                         │  │  └────┴────┴────┴────┴────┘ │ │
  Client ─── ACK ────────→ │  └──────────────────────────────┘ │
                         │                                   │
                         │  ┌──────────────────────────────┐ │
                         │  │    SYN Backlog (half-open)   │ │
                         │  │  ┌────┬────┬────┐           │ │
                         │  │  │SYN1│SYN2│SYN3│           │ │
                         │  │  └────┴────┴────┘           │ │
                         │  └──────────────────────────────┘ │
                         └──────────────────────────────────┘
                                    ↑
                            accept() lấy fd từ đây
```

### 6.3 `SOMAXCONN` vs explicit number

```c
// SOMAXCONN = giá trị tối đa OS cho phép
// Linux:     SOMAXCONN = 4096
// macOS:     SOMAXCONN = 128
// FreeBSD:   SOMAXCONN = 1024

listen(fd, SOMAXCONN);   // Dùng max của OS — project dùng cái này
listen(fd, 128);         // Dùng số cố định
```

**Sai lầm thường gặp:** Đặt backlog quá lớn không có nghĩa là server xử lý được nhanh hơn. Nếu application chậm `accept()`, backlog chỉ là bộ đệm tạm — nếu nó đầy, kernel sẽ bỏ qua SYN mới (không gửi SYN-ACK) → client retry.

---

## 7. `accept()` — Chấp nhận kết nối

### 7.1 Khái niệm

`accept()` lấy kết nối đầu tiên từ accept backlog của kernel và tạo ra một **socket hoàn toàn mới** dành riêng cho client đó.

```c
// src/server.c:557
int client_fd = accept(server->listen_fd, NULL, NULL);
```

### 7.2 Hai socket khác nhau

```
┌──────────────────────────────────────────────────────────────┐
│                                                              │
│  listen_fd (passive socket)                                  │
│  ┌────────────────────────────────────────────────────┐   │
│  │ state: LISTEN                                        │   │
│  │ Chỉ dùng để: listen() + accept()                 │   │
│  │ KHÔNG dùng để send/recv                           │   │
│  │ Tồn tại suốt vòng đời server                     │   │
│  └────────────────────────────────────────────────────┘   │
│                                                              │
│  client_fd (connected socket) ← accept() trả về              │
│  ┌────────────────────────────────────────────────────┐   │
│  │ state: ESTABLISHED                                 │   │
│  │ Dùng để: recv() + send() + close()              │   │
│  │ Mỗi client tạo MỘT client_fd riêng               │   │
│  │ close(client_fd) không ảnh hưởng listen_fd        │   │
│  └────────────────────────────────────────────────────┘   │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 7.3 Điều gì xảy ra bên trong `accept()`?

```
accept() bên trong kernel:
┌──────────────────────────────────────────────────────────────┐
│                                                              │
│  1. Kiểm tra accept backlog có kết nối nào không          │
│                                                              │
│  2. Nếu có:                                                │
│     → Tạo struct socket MỚI trong kernel                  │
│     → Gắn nó vào fd table của process → return client_fd  │
│     → Kernel giữ TCP state của kết nối này trong socket mới │
│                                                              │
│  3. Nếu không có (backlog rỗng):                         │
│     → Blocking mode: thread SLEEP cho đến khi có kết nối  │
│     → Non-blocking mode: return -1, errno = EAGAIN        │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 7.4 Không đồng bộ: `accept()` trong multi-threaded server

```c
// src/server.c:544–568
// Accept loop trong MAIN THREAD (single thread):
int server_run(server_t *server) {
    worker_context_t context;
    context.server = server;

    thread_pool_start(&server->pool, ...);  // Tạo worker threads TRƯỚC

    while (!server->should_stop) {
        int client_fd = accept(server->listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) continue;  // Signal interrupt → retry
            continue;
        }

        // Ngay lập tức enqueue, KHÔNG blocking ở đây
        queue_result_t result = socket_queue_enqueue(server->queue, client_fd);
        if (result == QUEUE_FULL) {
            send_simple_response(client_fd, NULL, 503, ...);
            close(client_fd);  // Backpressure
        }
    }
}
```

**Điểm quan trọng:** Acceptor thread chỉ làm 3 việc:

1. `accept()` — lấy kết nối
2. `enqueue()` — đặt vào queue
3. `close()` nếu queue full

Worker threads làm nặng nhọc: `recv()` + parse + `send()` + `close()`.

### 7.5 `getpeername()` — Lấy IP của client

```c
// src/server.c:136–151
static void get_client_ip(int client_fd, char *buffer, size_t buffer_size) {
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    if (getpeername(client_fd, (struct sockaddr *)&peer_addr, &peer_len) == 0 &&
        getnameinfo((struct sockaddr *)&peer_addr, peer_len,
                    buffer, (socklen_t)buffer_size, NULL, 0,
                    NI_NUMERICHOST) == 0) {
        return;  // Success: buffer chứa IP
    }

    snprintf(buffer, buffer_size, "-");  // Fallback
}
```

`getpeername()` lấy địa chỉ của peer (client). `getnameinfo()` chuyển `struct sockaddr` thành string (ví dụ: `"127.0.0.1"` hoặc `"::1"`).

---

## 8. `send()` / `recv()` — Truyền nhận dữ liệu

### 8.1 Byte-Stream semantics — Điểm hay nhầm lẫn nhất

**TCP không phải message protocol.** TCP là **byte-stream** — nó không có khái niệm "message boundary". Điều này có nghĩa:

```
Client gửi:
send(fd, "Hello", 5)    // 1 syscall
send(fd, " World", 6)   // 1 syscall

Server nhận:
recv(fd, buf, 100)      // Có thể nhận: "Hello World" (cả 11 bytes)
                         // Hoặc: "Hello Wo" (chỉ 8 bytes)
                         // Hoặc: "Hello WorldHello World" (nếu client gửi 2 lần)
```

**Trong project — recv() loop:**

```c
// src/server.c:107–134
while (1) {
    // Tìm xem header đã complete chưa (\r\n\r\n)
    if (find_header_end(buffer, *buffered, &header_length)) {
        // Header đã complete, parse nó
        extract_request_line(...);
        http_parse_request(...);

        // Shift phần còn lại về đầu buffer
        memmove(buffer, buffer + header_length, *buffered - header_length);
        *buffered -= header_length;
        return 1;
    }

    // Header chưa complete → tiếp tục recv()
    ssize_t received = recv(client_fd,
                             buffer + *buffered,
                             READ_BUFFER_SIZE - *buffered,
                             0);

    if (received < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (received == 0) {
        return *buffered == 0 ? 0 : -1;  // Client đóng connection
    }
    *buffered += (size_t)received;
}
```

### 8.2 `send_all()` — Đảm bảo gửi đủ bytes

```c
// src/server.c:31–50
static int send_all(int fd, const void *buffer, size_t length) {
    const char *data = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;  // Retry on signal
            return -1;
        }
        if (n == 0) {
            return -1;  // Connection closed unexpectedly
        }
        sent += (size_t)n;
    }
    return 0;
}
```

**Tại sao cần vòng lặp?**

```
send(fd, buf, 8192, 0) có thể trả về:
  → 8192 (gửi tất cả)       ← Thường xảy ra với local socket
  → 4096 (gửi một phần)     ← Có thể xảy ra khi TCP buffer đầy
  → -1 (lỗi)                 ← errno = EINTR, EAGAIN, ...

Vòng lặp đảm bảo: gửi ĐỦ bytes trước khi return
```

### 8.3 TCP Buffer và Flow Control

```
┌──────────────┐                         ┌──────────────┐
│  Client      │                         │   Server     │
│              │    TCP Buffer (TX)       │              │
│  App ───────→│ ────────────────────────│←──────────── App
│              │                         │              │
│              │    TCP Buffer (RX)       │              │
│  App ←────────│ ←──────────────────────│───────│     │
│              │                         │       ↓     │
└──────────────┘                         │   TCP Buffer│
                                         │   (filled) │
                                         └─────────────┘
```

**Flow control:** TCP có cơ chế ngăn sender gửi quá nhanh cho receiver. Receiver gửi `Window Size` trong mỗi ACK, nói cho sender biết "tôi còn chỗ bao nhiêu trong buffer". Nếu window = 0, sender phải chờ.

**Congestion control:** TCP cũng tự điều chỉnh tốc độ dựa trên network congestion (sử dụng thuật toán như CUBIC, Reno).

### 8.4 Flags trong `send()` / `recv()`

```c
// src/server.c:122
ssize_t received = recv(client_fd, buffer + *buffered,
                        READ_BUFFER_SIZE - *buffered, 0);
                        //                          ↑
                        //                     flags = 0 (default)

send(fd, buffer, length, 0);  // flags = 0

// Các flags phổ biến:
MSG_NOSIGNAL   // Không gửi SIGPIPE khi peer đóng connection
MSG_DONTWAIT  // Non-blocking (thay vì O_NONBLOCK)
MSG_OOB       // Out-of-band data (rare)
MSG_PEEK      // recv: nhìn trộm data mà không xóa khỏi buffer
```

---

## 9. `SO_REUSEADDR` — Tái sử dụng cổng

### 9.1 Vấn đề: "Address already in use"

```c
// src/server.c:501
int yes = 1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
```

Nếu không có `SO_REUSEADDR`, khi restart server:

```bash
$ ./httpd -p 8080 &
$ kill %1
$ ./httpd -p 8080
# Error: Address already in use (98)
# Chờ 1-4 phút (TIME_WAIT duration) rồi mới bind lại được
```

### 9.2 Tại sao xảy ra? — TCP TIME_WAIT

```
Khi connection đóng:

Client ──── FIN ────────→ Server
Client ←── ACK ───────── Server
Client ←── FIN ───────── Server  (Server close)
Client ─── ACK ────────→ Server

Client đợi 2*MSL rồi close hoàn toàn

Trong thời gian này:
- Connection vẫn ở TIME_WAIT trên CLIENT PORT
- Server không bind được port đó trong 1-4 phút
- MSL (Maximum Segment Lifetime) ≈ 60s trên Linux
```

```
CÓ SO_REUSEADDR:
  Server bind() → THÀNH CÔNG ngay
  Kernel cho phép bind vào port đang TIME_WAIT

KHÔNG CÓ SO_REUSEADDR:
  Server bind() → EADDRINUSE
  Phải đợi 2*MSL (thường 1-4 phút)
```

### 9.3 `SO_REUSEADDR` trên macOS

```bash
# macOS (Darwin) có thêm SO_REUSEPORT từ macOS 10.14
# SO_REUSEPORT: cho phép NHIỀU processes cùng bind() một port
# → Dùng cho load balancing ở tầng OS

setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
```

### 9.4 Cờ `SO_LINGER` (bonus)

```c
// Không dùng trong project nhưng liên quan:
struct linger ling;
ling.l_onoff = 1;   // Bật linger
ling.l_linger = 0;  // Đóng ngay lập tức, gửi RST thay vì FIN

setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
close(fd);
// → Gửi TCP RST, không làm TIME_WAIT
// → Dùng cho crash recovery: không muốn chờ TIME_WAIT
```

---

## 10. Blocking vs Non-blocking I/O

### 10.1 Blocking semantics

```c
// Blocking recv() — THREAD BỊ SLEEP cho đến khi có data
ssize_t n = recv(fd, buffer, 1024, 0);
// Khi no data:
//   Thread:   [  sleeping...  ]──[woken up]──[data ready]──[returned]
//   CPU:      [  0% usage    ]──[wake!]──[process data]──[back to sleep]

// Blocking accept() — THREAD BỊ SLEEP cho đến khi có connection
int client_fd = accept(listen_fd, NULL, NULL);
// Khi no connection:
//   Thread:   [  sleeping...  ]──[woken]──[client arrives]──[returned]
```

### 10.2 Trong project: Blocking + Thread Pool

```
┌──────────────────────────────────────────────────────────────┐
│  Thread Pool (blocking I/O)                                  │
│                                                               │
│  Main thread:                                                │
│    accept() ────────────────── [BLOCKING]                   │
│    enqueue()                                               │
│                                                               │
│  Worker 1:                                                  │
│    dequeue() ──────────────── [BLOCKING when queue empty]   │
│    recv() ─────────────────── [BLOCKING when no data]      │
│    send() ──────────────────── [BLOCKING when sending]      │
│    close()                                                 │
│    dequeue() ──────────────── [BLOCKING again...]           │
│                                                               │
│  Worker 2:  (same pattern)                                  │
│                                                               │
│  Worker N:  (same pattern)                                  │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

### 10.3 Alternative: Non-blocking + epoll (event-driven)

```c
// Cách event-driven (nginx, node.js dùng):
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

while (1) {
    // epoll_wait() trả về khi có event
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < n; i++) {
        if (events[i].events & EPOLLIN) {
            // Có data để đọc (non-blocking recv)
            while ((n = recv(fd, buf, size, MSG_DONTWAIT)) > 0) {
                process(buf, n);
            }
        }
        if (events[i].events & EPOLLOUT) {
            // Có thể gửi (non-blocking send)
            send(fd, data, len, MSG_DONTWAIT);
        }
    }
}
```

### 10.4 So sánh Blocking + Thread Pool vs Event-driven

| Aspect          | Blocking + Thread Pool (Project này)  | Event-driven (epoll/kqueue)          |
| --------------- | -------------------------------------- | ------------------------------------ |
| Threads         | N threads cho N concurrent clients     | 1 thread cho 1000+ clients           |
| Memory          | Mỗi thread có stack (~8MB)           | Stack nhỏ                           |
| CPU usage       | Context switch khi threads block       | Ít context switch                   |
| Code complexity | Simple, sequential logic               | Complex state machines               |
| Scaling         | Poor với 10K+ idle connections        | Excellent                            |
| Keep-Alive idle | Worker bị chiếm bởi idle connection | Handler nhẹ, return ngay            |
| Phù hợp       | Project nhỏ, < 1000 concurrent        | Production servers, 10K+ connections |

**Project này chọn Blocking + Thread Pool** vì:

1. Đơn giản để implement và hiểu
2. Phù hợp với quy mô project (không phải production server)
3. Mỗi worker xử lý 1 request hoàn chỉnh → clean sequential code

---

## 11. TCP State Machine & TIME_WAIT

### 11.1 TCP States

```
                                    ┌──────────┐
  Client                           │          │
  ┌──────┐     connect()          │  CLOSED  │
  │INIT  │ ──────────────────────────────→ │
  └──┬───┘                         │          │
     │                             └─────┬────┘
     │ SYN_sent                           │
     │ ──────── SYN ──────────────────→ │
     │ ←────── SYN-ACK ──────────────── │
     │                                    │
     │ SYN_received                        │
     │ ──────── ACK ──────────────────→ │
     │                                    │
     ▼                                    ▼
  ┌──────────┐                     ┌──────────┐
  │          │                     │          │
  │ESTABLISHED│←──────────────────│ESTABLISHED│
  │          │                     │          │
  └────┬─────┘                     └─────┬────┘
       │ FIN_wait_1                       │ FIN_wait_1
       │ ─────── FIN ────────────────→ │ ─────── FIN ──────────────→
       │ ←────── ACK ─────────────── │ ←────── ACK ─────────────
       │                                    │
       │ FIN_wait_2                        │ Closing
       │ ←────── FIN ──────────────── │ ─────── FIN ──────────────→
       │ ─────── ACK ────────────────→ │
       │                                    │
       │ ←────────────────────────────── │ TIME_WAIT
       │                                    │ (đợi 2*MSL)
       ▼                                    ▼
  ┌──────────┐                     ┌──────────┐
  │ CLOSED   │                     │ CLOSED   │
  └──────────┘                     └──────────┘
```

### 11.2 TIME_WAIT chi tiết

```
Server close() → gửi FIN → state = TIME_WAIT

TIME_WAIT kéo dài: 2 * MSL (Maximum Segment Lifetime)
  Linux:   MSL = 60s → TIME_WAIT = 120s
  macOS:   MSL = 30s → TIME_WAIT = 60s

Mục đích:
1. Chờ các packet đi lạc trên mạng đến (không bị nhận nhầm vào connection mới)
2. Đảm bảo remote đã nhận ACK cuối cùng

Nếu không có TIME_WAIT:
  Server gửi FIN, ACK cuối cùng
  Packet bị delay trên mạng
  Server restart, bind() cùng port
  Packet cũ đến → bị nhầm là data của connection mới → CORRUPTION
```

### 11.3 `SO_REUSEADDR` bỏ qua TIME_WAIT

```c
// Khi bind() với SO_REUSEADDR:
if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0) {
    bind(fd, ...);  // Bind THÀNH CÔNG ngay, bất chấp TIME_WAIT
}
```

**Đây là AN TOÀN vì:**

- TCP sequence numbers ngăn nhầm lẫn data giữa connections
- TIME_WAIT chỉ để cleanup, không phải security measure

---

## 12. Đi sâu vào TCP Buffer

### 12.1 Hai buffer trong kernel

```
TCP Connection (bidirectional):

  Client                              Server
    │ ──────────── TCP TX Buffer ────→│  (gửi đi)
    │ ←──────────── TCP RX Buffer ─────│  (nhận vào)

Mỗi socket có 2 buffer:
  TX (Transmit): App → network
  RX (Receive):  network → App
```

| Buffer    | Kernel Parameters | Mặc định Linux | Mặc định macOS |
| --------- | ----------------- | ----------------- | ----------------- |
| TX Buffer | `SO_SNDBUF`     | ~208KB            | ~256KB            |
| RX Buffer | `SO_RCVBUF`     | ~208KB            | ~256KB            |

### 12.2 Kiểm tra buffer size

```bash
# Linux
cat /proc/sys/net/ipv4/tcp_rmem   # RX: min default max
cat /proc/sys/net/ipv4/tcp_wmem   # TX: min default max

# macOS
sysctl net.inet.tcp.recvbuf_max
sysctl net.inet.tcp.sendbuf_max
```

### 12.3 Tác động trong project

```c
// HTTP server gửi file lớn:
while (remaining > 0) {
    size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t got = fread(buffer, 1, want, file);

    // send_all() → TCP TX Buffer → Network
    if (send_all(fd, buffer, got) != 0) {
        // Client đóng connection hoặc buffer đầy
        // Nếu TX Buffer đầy, send() block (blocking mode)
        break;
    }
    remaining -= got;
}
```

**Nếu client đọc chậm** (ví dụ: bandwidth thấp):

- TX Buffer đầy
- `send()` blocks hoặc trả về -1/EAGAIN (non-blocking)
- Worker bị blocked trong khi gửi file → throughput giảm

**Giải pháp trong production:**

- Dùng `sendfile()` (Linux) — zero-copy từ file descriptor sang socket, không qua user buffer
- Dùng async I/O (io_uring trên Linux)

---

## 13. Bigger Picture: HTTP trên TCP

### 13.1 Layering

```
┌──────────────────────────────────────────────────────────────┐
│  Layer 7: HTTP                                                │
│  "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"       │
│  Định dạng: plain text, CRLF-delimited                    │
└──────────────────────────────────────────────────────────────┘
                          │ serialize bytes
                          ▼
┌──────────────────────────────────────────────────────────────┐
│  Layer 4: TCP                                                │
│  • Reliable delivery (ACK/NACK)                              │
│  • Ordered byte stream                                        │
│  • Flow control (window size)                               │
│  • Congestion control                                        │
│  • Connection lifecycle (handshake/teardown)                 │
└──────────────────────────────────────────────────────────────┘
                          │ segment bytes
                          ▼
┌──────────────────────────────────────────────────────────────┐
│  Layer 3: IP                                                 │
│  • Routing (source/dest IP)                                 │
│  • Fragmentation & reassembly                               │
└──────────────────────────────────────────────────────────────┘
                          │ packet
                          ▼
┌──────────────────────────────────────────────────────────────┐
│  Layer 2: Ethernet / Wi-Fi                                   │
│  • MAC addresses                                             │
│  • Frames                                                   │
└──────────────────────────────────────────────────────────────┘
```

### 13.2 HTTP/1.0 vs HTTP/1.1 trên TCP

```
HTTP/1.0 without Keep-Alive:
  TCP handshake (1 RTT)
  HTTP Request (1 RTT)
  HTTP Response (1 RTT)
  TCP teardown (2 RTT)
  ──────────────────────────
  Total: 5 RTT cho 1 request

HTTP/1.1 with Keep-Alive (3 requests):
  TCP handshake (1 RTT)
  HTTP Request 1 + Response 1 (1 RTT)
  HTTP Request 2 + Response 2 (1 RTT)
  HTTP Request 3 + Response 3 (1 RTT)
  TCP teardown (2 RTT)
  ──────────────────────────
  Total: 6 RTT cho 3 requests → ~2 RTT/request
  So với HTTP/1.0: tiết kiệm 60% RTT overhead
```

### 13.3 Tại sao HTTP/2 / HTTP/3?

```
HTTP/1.1 pipeline (limited):
  Client ──→ GET /1 ──→ GET /2 ──→ GET /3 ──→
  Client ←── Response1 ←── Response2 ←── Response3 ←──

  Problem: Head-of-line blocking
  Nếu Response2 mất gói, Response3 phải chờ
  → Tất cả đợi

HTTP/2 multiplexed streams:
  Client ──→ Stream 1: GET /1 ──────────────────────→│
  Client ──→ Stream 2: GET /2 ──────→│
  Client ──→ Stream 3: GET /3 ──→│
  Client ←── Stream 2: Response2 ←─│  (giải quyết HOL blocking)
  Client ←── Stream 1: Response1 ←───────────────────│
  Client ←── Stream 3: Response3 ←─│
```

### 13.4 Trong project: Blocking TCP cho worker thread

```
┌─────────────────────────────────────────────────────────────┐
│  Worker Thread N                                           │
│                                                             │
│  recv(client_fd, buffer, 8192, 0)                        │
│    ↓                                                        │
│    TCP RX Buffer ──→ buffer (user space)                  │
│    ↓                                                        │
│    http_parse_request(buffer, ...)                         │
│    ↓                                                        │
│    file_resolve_path(...)                                 │
│    ↓                                                        │
│    fread(file, buffer, 8192)                             │
│    ↓                                                        │
│    TCP TX Buffer ←── buffer (user space)                  │
│    send(client_fd, buffer, n, 0)                         │
│    ↓                                                        │
│    TCP RX/TX Buffer có thể blocked thread                  │
│    (nhưng worker khác vẫn chạy)                           │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## Tổng kết Level 2 — Quick Reference

```
┌─────────────────────────────────────────────────────────────┐
│ SOCKET LIFECYCLE                                            │
│                                                             │
│  socket(AF_INET, SOCK_STREAM, 0) → fd                   │
│  getaddrinfo(host, port, &hints, &result)               │
│  setsockopt(fd, SO_REUSEADDR) ← restart nhanh            │
│  bind(fd, sockaddr, len)                                  │
│  listen(fd, SOMAXCONN)  ← backlog queue                    │
│  accept(fd) → NEW fd cho mỗi client                       │
│  recv(fd, buf, n, 0)   ← blocking read                    │
│  send(fd, buf, n, 0)   ← blocking write                   │
│  close(fd)              ← đóng connection                  │
│                                                             │
│  EINTR: retry nếu syscall bị interrupt bởi signal          │
│  EAGAIN: non-blocking mode — không có data sẵn sàng       │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ TCP 3-WAY HANDSHAKE                                       │
│                                                             │
│  Client ──── SYN ────────────→ Server                     │
│  Client ←─── SYN-ACK ───────── Server                     │
│  Client ──── ACK ────────────→ Server                     │
│  ────────────────────────────────────                     │
│  1 RTT trước khi gửi byte đầu tiên                       │
│  → Keep-Alive giữ kết nối, tránh handshake lại           │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ TCP BYTE-STREAM SEMANTICS                                  │
│                                                             │
│  send() 1 lần ≠ recv() 1 lần                            │
│  recv() có thể nhận 1 phần hoặc nhiều messages cùng lúc │
│  → Luôn dùng vòng lặp recv() để đọc cho đến khi đủ     │
│  → Luôn dùng vòng lặp send() để gửi cho đến khi hết    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ TCP STATE MACHINE                                           │
│                                                             │
│  CLOSED → SYN_sent → ESTABLISHED → FIN_wait → CLOSED     │
│                                                             │
│  TIME_WAIT: đợi 2*MSL sau khi close                    │
│  SO_REUSEADDR: bind vào port đang TIME_WAIT               │
│  SOMAXCONN: backlog = số kết nối chờ accept()            │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ BLOCKING vs EVENT-DRIVEN                                  │
│                                                             │
│  Blocking: thread sleep khi chờ I/O                       │
│  → Đơn giản, dùng thread pool để concurrency              │
│  → Phù hợp: project nhỏ, < 1000 clients                  │
│                                                             │
│  Event-driven (epoll/kqueue): 1 thread quản nhiều FDs   │
│  → Phức tạp hơn, cần state machines                       │
│  → Phù hợp: production, 10K+ idle connections             │
│                                                             │
│  SO_REUSEADDR: tái sử dụng port ngay sau restart         │
└─────────────────────────────────────────────────────────────┘
```
