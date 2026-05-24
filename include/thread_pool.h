#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>

typedef enum {
    QUEUE_OK = 0,
    QUEUE_FULL = 1,
    QUEUE_CLOSED = 2,
    QUEUE_ERROR = 3
} queue_result_t;

typedef struct socket_queue socket_queue_t;

typedef void (*thread_pool_handler_t)(int client_fd, void *context);

typedef struct {
    socket_queue_t *queue;
    void *threads;
    int thread_count;
    thread_pool_handler_t handler;
    void *handler_context;
} thread_pool_t;

int socket_queue_init(socket_queue_t **queue, size_t capacity);
void socket_queue_destroy(socket_queue_t *queue);
queue_result_t socket_queue_enqueue(socket_queue_t *queue, int client_fd);
queue_result_t socket_queue_dequeue(socket_queue_t *queue, int *client_fd);
void socket_queue_shutdown(socket_queue_t *queue);

int thread_pool_start(thread_pool_t *pool, socket_queue_t *queue, int thread_count, thread_pool_handler_t handler, void *context);
void thread_pool_stop(thread_pool_t *pool);

#endif
