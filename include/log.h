#ifndef LOG_H
#define LOG_H

#include <stddef.h>

typedef struct access_log access_log_t;

int access_log_open(access_log_t **log, const char *path);
void access_log_close(access_log_t *log);
void access_log_write(access_log_t *log, const char *client_ip, const char *request_line, int status_code, size_t bytes_sent);

#endif
