#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct access_log {
    FILE *file;
    pthread_mutex_t mutex;
};

int access_log_open(access_log_t **log, const char *path) {
    access_log_t *created;

    if (log == NULL || path == NULL) {
        return -1;
    }

    created = calloc(1, sizeof(*created));
    if (created == NULL) {
        return -1;
    }

    created->file = fopen(path, "a");
    if (created->file == NULL) {
        free(created);
        return -1;
    }

    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        fclose(created->file);
        free(created);
        return -1;
    }

    *log = created;
    return 0;
}

void access_log_close(access_log_t *log) {
    if (log == NULL) {
        return;
    }
    pthread_mutex_destroy(&log->mutex);
    fclose(log->file);
    free(log);
}

void access_log_write(access_log_t *log, const char *client_ip, const char *request_line, int status_code, size_t bytes_sent) {
    time_t now;
    struct tm local_time;
    char timestamp[64];

    if (log == NULL || client_ip == NULL || request_line == NULL) {
        return;
    }

    now = time(NULL);
    /* Sử dụng localtime_r (phiên bản reentrant) thay vì localtime để đảm bảo an toàn đa luồng, 
       kết quả ghi nhận trực tiếp vào struct cục bộ local_time nằm trên Stack thay vì static buffer dùng chung */
    localtime_r(&now, &local_time);
    strftime(timestamp, sizeof(timestamp), "%d/%b/%Y:%H:%M:%S %z", &local_time);

    /* Khóa mutex bảo vệ việc ghi log vào file chung giữa các luồng worker */
    pthread_mutex_lock(&log->mutex);
    fprintf(log->file, "%s - - [%s] \"%s\" %d %zu\n",
            client_ip, timestamp, request_line, status_code, bytes_sent);
    /* Đẩy dữ liệu tức thời từ buffer của C xuống ổ cứng để đảm bảo log không bị mất nếu server crash */
    fflush(log->file);
    /* Mở khóa mutex để các luồng khác có thể ghi log tiếp theo */
    pthread_mutex_unlock(&log->mutex);
}
