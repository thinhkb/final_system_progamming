#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_HOST "0.0.0.0"
#define DEFAULT_PORT 8080
#define DEFAULT_THREAD_COUNT 4
#define DEFAULT_QUEUE_CAPACITY 64
#define DEFAULT_DOC_ROOT "www"
#define DEFAULT_ACCESS_LOG "access.log"
#define READ_BUFFER_SIZE 8192
#define RESPONSE_BUFFER_SIZE 8192

typedef struct {
    const char *host;
    int port;
    int thread_count;
    int queue_capacity;
    const char *doc_root;
    const char *access_log;
} server_config_t;

#endif
