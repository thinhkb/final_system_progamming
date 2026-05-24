#include "thread_pool.h"

#include <pthread.h>
#include <stdio.h>

#define ASSERT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)

typedef struct {
    socket_queue_t *queue;
    queue_result_t result;
    int value;
} dequeue_args_t;

static void *blocking_dequeue(void *arg) {
    dequeue_args_t *args = arg;
    args->result = socket_queue_dequeue(args->queue, &args->value);
    return NULL;
}

static int test_fifo_and_full(void) {
    socket_queue_t *queue = NULL;
    int value = 0;
    ASSERT_TRUE(socket_queue_init(&queue, 2) == 0);
    ASSERT_TRUE(socket_queue_enqueue(queue, 10) == QUEUE_OK);
    ASSERT_TRUE(socket_queue_enqueue(queue, 11) == QUEUE_OK);
    ASSERT_TRUE(socket_queue_enqueue(queue, 12) == QUEUE_FULL);
    ASSERT_TRUE(socket_queue_dequeue(queue, &value) == QUEUE_OK);
    ASSERT_TRUE(value == 10);
    ASSERT_TRUE(socket_queue_dequeue(queue, &value) == QUEUE_OK);
    ASSERT_TRUE(value == 11);
    socket_queue_destroy(queue);
    return 0;
}

static int test_shutdown_wakes_consumer(void) {
    socket_queue_t *queue = NULL;
    pthread_t thread;
    dequeue_args_t args;

    ASSERT_TRUE(socket_queue_init(&queue, 1) == 0);
    args.queue = queue;
    args.result = QUEUE_ERROR;
    args.value = -1;

    ASSERT_TRUE(pthread_create(&thread, NULL, blocking_dequeue, &args) == 0);
    socket_queue_shutdown(queue);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE(args.result == QUEUE_CLOSED);
    socket_queue_destroy(queue);
    return 0;
}

int main(void) {
    ASSERT_TRUE(test_fifo_and_full() == 0);
    ASSERT_TRUE(test_shutdown_wakes_consumer() == 0);
    puts("unit_thread_pool: PASS");
    return 0;
}
