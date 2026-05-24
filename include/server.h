#ifndef SERVER_H
#define SERVER_H

#include "config.h"

#include "thread_pool.h"

#include <signal.h>

typedef struct server {
    server_config_t config;
    int listen_fd;
    volatile sig_atomic_t should_stop;
    socket_queue_t *queue;
    thread_pool_t pool;
} server_t;

int server_init(server_t *server, const server_config_t *config);
int server_run(server_t *server);
void server_stop(server_t *server);
void server_destroy(server_t *server);

#endif
