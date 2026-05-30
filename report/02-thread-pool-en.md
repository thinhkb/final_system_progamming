# Thread Pool Implementation
## Technical Phase Report

---

## 1. Overview

This document analyzes the thread pool implementation in the multi-threaded HTTP file server, covering the producer-consumer pattern, bounded queue synchronization, thread lifecycle management, backpressure handling, and graceful shutdown. All code references are from `src/thread_pool.c` and `src/server.c`.

---

## 2. Why a Thread Pool?

### 2.1 The Thread-per-Connection Problem

Creating a new thread for each incoming connection has several drawbacks:

| Approach | Thread per Connection | Fixed Thread Pool |
|----------|----------------------|-------------------|
| **Thread creation cost** | High: `pthread_create()` per request | One-time cost at startup |
| **Memory usage** | Unbounded: can exhaust memory under load | Bounded by pool size |
| **Context switching** | High at scale | Controlled |
| **Responsiveness under burst** | May succeed briefly, then OOM | Backpressure via bounded queue |
| **Synchronization complexity** | Simple (one thread per socket) | Moderate (queue + mutex + CVs) |

### 2.2 Fixed Pool Advantages

A fixed thread pool with N pre-created threads:
- **Eliminates thread creation overhead** for each request
- **Limits concurrency** to a predictable maximum
- **Provides natural load distribution** via the queue's FIFO ordering
- **Simplifies resource accounting** — maximum threads is known at startup

The trade-off is that if all N threads are busy, new connections must wait. This is handled by the bounded queue, which applies **backpressure** when the system is overloaded.

---

## 3. Producer-Consumer Pattern

### 3.1 Architecture

```
┌──────────────┐    enqueue()     ┌─────────────────────────┐    dequeue()    ┌────────────┐
│  Acceptor    │ ───────────────→ │   Bounded Socket Queue  │ ──────────────→ │  Worker 1  │
│  (producer)  │                  │  (circular buffer +     │                 └────────────┘
└──────────────┘                  │   mutex + 2 condvars)    │    dequeue()    ┌────────────┐
                                   │                          │ ──────────────→ │  Worker 2  │
                                   │  head ──────────────── tail               └────────────┘
                                   │                          │                 ...
                                   └──────────────────────────┘    dequeue()    ┌────────────┐
                                                                                 │  Worker N  │
                                                                                 └────────────┘
```

The acceptor thread (in `server_run()` at `src/server.c:544`) runs an infinite loop, accepting connections and enqueueing them:

```557:568:src/server.c
    while (!server->should_stop) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        queue_result_t enqueue_result;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server->should_stop) {
                break;
            }
            continue;
        }

        enqueue_result = socket_queue_enqueue(server->queue, client_fd);
```

Worker threads (in `worker_main()` at `src/thread_pool.c:22`) run a dequeue loop:

```22:34:src/thread_pool.c
static void *worker_main(void *arg) {
    worker_args_t *worker_args = arg;
    thread_pool_t *pool = worker_args->pool;
    int client_fd = -1;

    free(worker_args);

    while (socket_queue_dequeue(pool->queue, &client_fd) == QUEUE_OK) {
        pool->handler(client_fd, pool->handler_context);
    }

    return NULL;
}
```

---

## 4. Bounded Queue Design

### 4.1 Data Structure

The `socket_queue_t` (`src/thread_pool.c:6`) is a circular buffer with synchronization primitives:

```6:16:src/thread_pool.c
struct socket_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int shutdown;
};
```

| Field | Purpose |
|-------|---------|
| `items` | Circular buffer storing file descriptors |
| `head` | Index of next item to dequeue |
| `tail` | Index where next item will be enqueued |
| `count` | Current number of items in queue |
| `capacity` | Maximum queue size |
| `shutdown` | Flag set during shutdown to unblock waiters |
| `mutex` | Protects all shared state |
| `not_empty` | Signaled when items are available |
| `not_full` | Signaled when space becomes available |

### 4.2 Circular Buffer Mechanics

The circular buffer wraps indices using modulo arithmetic:

```103:106:src/thread_pool.c
    } else {
        queue->items[queue->tail] = client_fd;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
```

```128:130:src/thread_pool.c
    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
```

### 4.3 Enqueue Operation

`socket_queue_enqueue()` (`src/thread_pool.c:90`):

```90:111:src/thread_pool.c
queue_result_t socket_queue_enqueue(socket_queue_t *queue, int client_fd) {
    queue_result_t result = QUEUE_OK;

    if (queue == NULL) {
        return QUEUE_ERROR;
    }

    pthread_mutex_lock(&queue->mutex);
    if (queue->shutdown) {
        result = QUEUE_CLOSED;
    } else if (queue->count == queue->capacity) {
        result = QUEUE_FULL;
    } else {
        queue->items[queue->tail] = client_fd;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
    }
    pthread_mutex_unlock(&queue->mutex);

    return result;
}
```

Key behaviors:
- Returns `QUEUE_FULL` immediately if at capacity (non-blocking enqueue)
- Returns `QUEUE_CLOSED` if queue is already shut down
- Signals `not_empty` to wake one waiting worker

### 4.4 Dequeue Operation

`socket_queue_dequeue()` (`src/thread_pool.c:113`):

```113:135:src/thread_pool.c
queue_result_t socket_queue_dequeue(socket_queue_t *queue, int *client_fd) {
    if (queue == NULL || client_fd == NULL) {
        return QUEUE_ERROR;
    }

    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return QUEUE_CLOSED;
    }

    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return QUEUE_OK;
}
```

Key behaviors:
- **Waits** on `not_empty` while the queue is empty (blocking dequeue)
- **Exits** when queue is empty AND shutdown flag is set
- Signals `not_full` to wake a waiting producer

---

## 5. Backpressure Mechanism

### 5.1 When the Queue is Full

When the acceptor tries to enqueue and the queue is at capacity, it receives `QUEUE_FULL`:

```570:578:src/server.c
        enqueue_result = socket_queue_enqueue(server->queue, client_fd);
        if (enqueue_result == QUEUE_FULL) {
            response_result_t response = {0, 0};
            char client_ip[128];

            get_client_ip(client_fd, client_ip, sizeof(client_ip));
            send_simple_response(client_fd, NULL, 503, "Service Unavailable\n", 0, &response);
            access_log_write(server->access_log, client_ip, "-", response.status_code, response.body_bytes);
            close(client_fd);
```

The client receives **HTTP 503 Service Unavailable** with `Connection: close`. This is the backpressure signal:

- Prevents unbounded memory growth under load
- Informs the client that the server is temporarily overloaded
- Preserves fairness — the connection is immediately closed rather than blocking

### 5.2 Queue Capacity

The queue capacity is configured at startup via the `queue_capacity` configuration option. This allows tuning based on expected load and available memory.

---

## 6. Graceful Shutdown

### 6.1 Shutdown Sequence

The shutdown sequence follows these steps:

```
server_stop() called
  → set should_stop = 1
  → close(listen_fd)           // Stop accepting new connections
  → socket_queue_shutdown()    // Signal shutdown flag + broadcast CVs
  
thread_pool_stop()
  → socket_queue_shutdown()    // (called again, idempotent)
  → for each thread: pthread_join()
  → free thread array
```

### 6.2 Queue Shutdown

`socket_queue_shutdown()` (`src/thread_pool.c:137`):

```137:147:src/thread_pool.c
void socket_queue_shutdown(socket_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}
```

**Critical**: Both condition variables are broadcast:
- `not_empty`: unblocks workers waiting on an empty queue
- `not_full`: unblocks the acceptor if it were ever to wait (currently non-blocking, but the signal is present for future use)

### 6.3 Worker Thread Exit

When a worker wakes up from `dequeue()` and finds the queue empty with shutdown flag set, it returns `QUEUE_CLOSED` and exits the loop:

```29:31:src/thread_pool.c
    while (socket_queue_dequeue(pool->queue, &client_fd) == QUEUE_OK) {
        pool->handler(client_fd, pool->handler_context);
    }
```

The `pthread_join()` in `thread_pool_stop()` (`src/thread_pool.c:186`) ensures all threads have exited before the function returns:

```193:197:src/thread_pool.c
    socket_queue_shutdown(pool->queue);
    threads = pool->threads;
    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
```

---

## 7. Thread Lifecycle

### 7.1 Startup

```167:181:src/thread_pool.c
    for (int i = 0; i < thread_count; i++) {
        worker_args_t *args = malloc(sizeof(*args));
        if (args == NULL) {
            pool->thread_count = i;
            thread_pool_stop(pool);
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
```

Each thread receives its own `worker_args_t` struct (allocated on heap, freed by the thread itself), which contains a pointer to the shared `thread_pool_t`.

### 7.2 Normal Operation

1. Worker calls `socket_queue_dequeue()` — blocks on `not_empty` CV if queue is empty
2. When socket is dequeued, worker calls `handler(client_fd, context)`
3. Handler processes the HTTP request, sends response, closes socket
4. Worker loops back to step 1

### 7.3 Shutdown

1. Acceptor stops accepting, closes listening socket
2. Queue shutdown flag is set, both CVs are broadcast
3. All waiting workers wake up
4. Workers with items process them normally
5. Workers with empty queue see shutdown flag and return `QUEUE_CLOSED`
6. Workers exit, main thread joins all of them
7. Resources are freed

---

## 8. Synchronization Invariants

| Invariant | Protection |
|-----------|-----------|
| Queue `count`, `head`, `tail` access | Always under `mutex` |
| Condition variable waits | Always under `mutex`, atomically releases it during wait |
| `shutdown` flag write | Under `mutex` |
| `shutdown` flag read | Under `mutex` (in dequeue) or happens-before (in enqueue check) |

### The `pthread_cond_wait` Pattern

```119:121:src/thread_pool.c
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
```

This pattern is correct because:
1. The `while` condition is checked **before** waiting
2. `pthread_cond_wait` **atomically** releases the mutex and starts waiting
3. When signaled, the mutex is re-acquired and the `while` loop runs again

Using `while` (not `if`) prevents **spurious wakeups** — a common POSIX behavior where `pthread_cond_signal` may wake a thread even without a signal being sent.

---

## 9. Summary

The thread pool implementation provides:

- **Producer-consumer separation** between acceptor and worker threads
- **Bounded queue** with mutex + two condition variables for efficient synchronization
- **Backpressure** via immediate 503 responses when the queue is full
- **Graceful shutdown** with broadcast signaling and joined thread termination
- **Non-blocking enqueue** for predictable worst-case acceptor latency
- **FIFO ordering** via circular buffer semantics

The design is deadlock-free because the acceptor never waits for a worker, and workers only hold the mutex briefly during queue operations.
