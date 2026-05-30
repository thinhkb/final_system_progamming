# Thread Pool — Tóm tắt phỏng vấn

---

## Tóm tắt

Server sử dụng **fixed thread pool** (số lượng thread cố định) kết hợp với **bounded queue** (hàng đợi có giới hạn) theo mô hình producer-consumer. Acceptor thread đóng vai trò producer, worker threads đóng vai trò consumer. Queue có giới hạn giúp kiểm soát backpressure khi server quá tải.

---

## Kiến trúc tổng quan

```
Client TCP connect
        │
        ▼
┌───────────────────┐
│  Acceptor thread  │  ← Producer: accept() → enqueue()
│  (server_run)     │
└────────┬──────────┘
         │ enqueue (có giới hạn kích thước)
         ▼
┌───────────────────────────────────┐
│  Bounded Socket Queue            │
│  • Circular buffer              │
│  • Mutex + 2 condition vars     │
│  • max N items                  │
└────────┬──────────────────────────┘
         │ dequeue
    ┌────┴────┬──────────┐
    ▼         ▼          ▼
┌────────┐ ┌────────┐ ┌────────┐
│Worker 1│ │Worker 2│ │Worker N│
│handle_ │ │handle_ │ │handle_ │
│client()│ │client()│ │client()│
└────────┘ └────────┘ └────────┘
```

---

## Producer-Consumer Pattern

### Ai là producer, ai là consumer?

- **Producer**: Acceptor thread trong `server_run()` — nhận kết nối TCP mới, bỏ vào queue
- **Consumer**: Các worker threads — lấy socket từ queue, xử lý HTTP request

### Luồng hoạt động

1. Client kết nối → `accept()` trả về `client_fd`
2. Acceptor → `socket_queue_enqueue(queue, client_fd)`
3. Worker → `socket_queue_dequeue(queue, &client_fd)`
4. Worker → `handle_client(client_fd, ...)` — parse HTTP, serve file, log
5. Worker đóng `client_fd`, quay lại bước 3

---

## Bounded Queue hoạt động thế nào?

### Cấu trúc dữ liệu

```c
struct socket_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;   // có item để lấy
    pthread_cond_t not_full;     // có chỗ trống
    int *items;                  // circular buffer
    size_t capacity;             // kích thước tối đa
    size_t head, tail, count;
    int shutdown;                // flag báo shutdown
};
```

### Enqueue (Producer gọi)

- Lock mutex
- Nếu queue đầy → trả về `QUEUE_FULL` (không blocking)
- Nếu queue shutdown → trả về `QUEUE_CLOSED`
- Ngược lại → thêm vào buffer, signal `not_empty`

### Dequeue (Consumer gọi)

- Lock mutex
- Nếu queue rỗng và chưa shutdown → `pthread_cond_wait()` trên `not_empty`
- Nếu queue rỗng VÀ shutdown → trả về `QUEUE_CLOSED` (worker exit)
- Ngược lại → lấy item từ head, signal `not_full`

### Tại sao dùng circular buffer?

Circular buffer cho phép reuse vị trí trong mảng thay vì shift mọi thứ khi dequeue. Chỉ cần di chuyển `head` và `tail` index (modulo capacity).

---

## Tại sao dùng Fixed Thread Pool?

### So với tạo thread mới cho mỗi connection

| | Thread-per-connection | Fixed thread pool |
|---|---|---|
| **Chi phí tạo thread** | Cao (mỗi request đều phải create) | Một lần lúc khởi động |
| **Bộ nhớ** | Không giới hạn (có thể OOM) | Giới hạn bởi pool size |
| **Responsive khi burst** | Tạm thời ổn → crash khi quá tải | Backpressure tự nhiên |
| **Độ phức tạp sync** | Thấp | Trung bình |

### Backpressure

Khi queue đầy, `enqueue()` trả `QUEUE_FULL`. Server gửi HTTP **503 Service Unavailable** và đóng kết nối ngay lập tức. Client biết server đang quá tải và có thể retry sau.

---

## Shutdown graceful

### Thứ tự shutdown

```
server_stop() được gọi
  1. should_stop = 1
  2. close(listen_fd)          // Không accept nữa
  3. socket_queue_shutdown()
       → shutdown = 1
       → broadcast not_empty     // Wake all waiting workers
       → broadcast not_full

thread_pool_stop()
  4. join all threads            // Chờ mọi worker exit
  5. free thread array
```

### Worker exit như thế nào?

Worker loop:
```c
while (socket_queue_dequeue(queue, &client_fd) == QUEUE_OK) {
    handler(client_fd, context);  // Xử lý request
}
// Queue empty + shutdown flag → QUEUE_CLOSED → exit
```

Khi shutdown flag được set và queue rỗng, `dequeue()` trả `QUEUE_CLOSED`, worker exit khỏi loop.

---

## Key Takeaways

### 1. Mutex + Condition Variables pattern

Luôn dùng `while` thay vì `if` khi chờ condition variable:

```c
while (queue->count == 0 && !queue->shutdown) {
    pthread_cond_wait(&not_empty, &mutex);
}
```

Lý do: tránh **spurious wakeup** (thread có thể wakeup mà không có signal thật sự).

### 2. Non-blocking enqueue

Acceptor **không bao giờ block** khi queue đầy. Nếu queue full → gửi 503 → close socket. Điều này đảm bảo acceptor không bị stuck khi system quá tải.

### 3. Broadcast khi shutdown

Cả `not_empty` VÀ `not_full` đều được broadcast để:
- Wake waiting workers (not_empty)
- Prepare for future blocking enqueue (not_full) — dù hiện tại enqueue không block

### 4. Shutdown flag check

`shutdown` flag được check **trong mutex** trong `dequeue()` nhưng chỉ là read trong `enqueue()`. Điều này an toàn vì:
- Write `shutdown` xảy ra trước khi join (happens-before)
- Mọi thread đều thấy giá trị đúng sau khi `pthread_join()` trả về

### 5. Mutex giải thích đơn giản

Mutex giống như một cái khóa cửa:
- Muốn đọc/ghi queue → phải lock mutex trước
- Xong → unlock
- Nếu khóa đang bị người khác giữ → chờ ở cửa

Condition variable giống như chuông:
- Khi queue rỗng, worker gọi `wait()` — "gọi tôi khi có việc"
- Producer khi thêm item vào → gọi `signal()` — "có việc đây, một người vào đi"
- Worker được wakeup, lock mutex, check lại queue có item không

---

## Câu hỏi phỏng vấn thường gặp

**Q: Tại sao không dùng thread pool không giới hạn?**

A: Không giới hạn = có thể tạo vô số thread khi có nhiều request đồng thời → hết bộ nhớ (OOM), quá nhiều context switch. Fixed pool giới hạn số lượng thread đồng thời.

**Q: Điều gì xảy ra nếu tất cả worker đều busy?**

A: Chúng tiếp tục xử lý request. Queue tích lũy items. Nếu queue đầy → 503 response cho connection mới. Khi một worker free → dequeue → xử lý.

**Q: Có deadlock potential không?**

A: Không, trong thiết kế này. Acceptor không bao giờ chờ worker. Worker chỉ giữ mutex trong thời gian rất ngắn (lấy/đặt item vào queue). Handler chạy bên ngoài mutex.

**Q: pthread_cond_wait hoạt động thế nào?**

A: `wait()` atomically unlock mutex và sleep cho đến khi được signal. Khi wake:
1. Lock lại mutex
2. Kiểm tra điều kiện (trong while loop)
3. Tiếp tục

Luôn dùng while loop, không dùng if, để xử lý spurious wakeup.
