#include "thread_pool.h"

#include <pthread.h>
#include <stdlib.h>

struct socket_queue {
    pthread_mutex_t mutex;     /* Khóa dùng để đồng bộ hóa việc truy cập hàng đợi giữa các luồng */
    pthread_cond_t not_empty;  /* Biến điều kiện báo hiệu hàng đợi KHÔNG rỗng (để đánh thức luồng worker) */
    pthread_cond_t not_full;   /* Biến điều kiện báo hiệu hàng đợi KHÔNG đầy (hiện tại không block luồng chính) */
    int *items;                /* Mảng động chứa các socket descriptor (client_fd) */
    size_t capacity;           /* Sức chứa tối đa của hàng đợi */
    size_t head;               /* Chỉ số đầu hàng đợi (lấy phần tử ra ở đây) */
    size_t tail;               /* Chỉ số cuối hàng đợi (thêm phần tử vào ở đây) */
    size_t count;              /* Số lượng kết nối hiện có trong hàng đợi */
    int shutdown;              /* Cờ báo hiệu đóng hàng đợi khi server dừng */
};

typedef struct {
    thread_pool_t *pool;
} worker_args_t;

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

int socket_queue_init(socket_queue_t **queue, size_t capacity) {
    socket_queue_t *created;

    if (queue == NULL || capacity == 0) {
        return -1;
    }

    created = calloc(1, sizeof(*created));
    if (created == NULL) {
        return -1;
    }

    created->items = calloc(capacity, sizeof(*created->items));
    if (created->items == NULL) {
        free(created);
        return -1;
    }

    created->capacity = capacity;
    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        free(created->items);
        free(created);
        return -1;
    }
    if (pthread_cond_init(&created->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&created->mutex);
        free(created->items);
        free(created);
        return -1;
    }
    if (pthread_cond_init(&created->not_full, NULL) != 0) {
        pthread_cond_destroy(&created->not_empty);
        pthread_mutex_destroy(&created->mutex);
        free(created->items);
        free(created);
        return -1;
    }

    *queue = created;
    return 0;
}

void socket_queue_destroy(socket_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    free(queue);
}

queue_result_t socket_queue_enqueue(socket_queue_t *queue, int client_fd) {
    queue_result_t result = QUEUE_OK;

    if (queue == NULL) {
        return QUEUE_ERROR;
    }

    /* Khóa mutex trước khi thay đổi dữ liệu dùng chung của hàng đợi */
    pthread_mutex_lock(&queue->mutex);
    if (queue->shutdown) {
        result = QUEUE_CLOSED;
    } else if (queue->count == queue->capacity) {
        /* Hàng đợi đầy: không block luồng chính mà trả về mã QUEUE_FULL để phản hồi 503 lập tức */
        result = QUEUE_FULL;
    } else {
        /* Thêm socket của client vào cuối hàng đợi vòng (circular queue) */
        queue->items[queue->tail] = client_fd;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        /* Phát tín hiệu đánh thức một worker thread đang ngủ do hàng đợi rỗng */
        pthread_cond_signal(&queue->not_empty);
    }
    /* Giải phóng khóa mutex để các luồng khác có thể truy cập */
    pthread_mutex_unlock(&queue->mutex);

    return result;
}

queue_result_t socket_queue_dequeue(socket_queue_t *queue, int *client_fd) {
    if (queue == NULL || client_fd == NULL) {
        return QUEUE_ERROR;
    }

    /* Khóa mutex trước khi đọc/ghi dữ liệu dùng chung */
    pthread_mutex_lock(&queue->mutex);
    
    /* Sử dụng vòng lặp while để tránh hiện tượng đánh thức giả (Spurious Wakeup) 
       và phòng trường hợp luồng khác nhảy vào lấy mất phần tử trước khi luồng này kịp khóa mutex */
    while (queue->count == 0 && !queue->shutdown) {
        /* Giải phóng mutex và đi vào giấc ngủ, chờ signal not_empty từ luồng chính */
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    /* Nếu hàng đợi trống và server đang dừng, mở khóa mutex và báo hàng đợi đã đóng */
    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return QUEUE_CLOSED;
    }

    /* Lấy socket ra khỏi đầu hàng đợi vòng */
    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    
    /* Phát tín hiệu báo hàng đợi đã có chỗ trống */
    pthread_cond_signal(&queue->not_full);
    
    /* Giải phóng khóa mutex */
    pthread_mutex_unlock(&queue->mutex);

    return QUEUE_OK;
}

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

int thread_pool_start(thread_pool_t *pool, socket_queue_t *queue, int thread_count, thread_pool_handler_t handler, void *context) {
    pthread_t *threads;

    if (pool == NULL || queue == NULL || thread_count <= 0 || handler == NULL) {
        return -1;
    }

    threads = calloc((size_t)thread_count, sizeof(*threads));
    if (threads == NULL) {
        return -1;
    }

    pool->queue = queue;
    pool->threads = threads;
    pool->thread_count = thread_count;
    pool->handler = handler;
    pool->handler_context = context;

    /* Khởi tạo trước một số lượng luồng làm việc (Worker Threads) cố định lúc khởi chạy */
    for (int i = 0; i < thread_count; i++) {
        worker_args_t *args = malloc(sizeof(*args));
        if (args == NULL) {
            pool->thread_count = i;
            thread_pool_stop(pool);
            return -1;
        }
        args->pool = pool;
        /* Tạo luồng mới chạy hàm worker_main */
        if (pthread_create(&threads[i], NULL, worker_main, args) != 0) {
            free(args);
            pool->thread_count = i;
            thread_pool_stop(pool);
            return -1;
        }
    }

    return 0;
}

void thread_pool_stop(thread_pool_t *pool) {
    pthread_t *threads;

    if (pool == NULL || pool->threads == NULL) {
        return;
    }

    socket_queue_shutdown(pool->queue);
    threads = pool->threads;
    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    pool->threads = NULL;
    pool->thread_count = 0;
}
