#ifndef FILES_H
#define FILES_H

#include <stddef.h>

#define FILE_PATH_MAX 4096

typedef enum {
    FILE_RESULT_OK,
    FILE_RESULT_NOT_FOUND,
    FILE_RESULT_FORBIDDEN,
    FILE_RESULT_ERROR
} file_result_t;

typedef enum {
    FILE_KIND_REGULAR,
    FILE_KIND_DIRECTORY
} file_kind_t;

typedef struct {
    char resolved_path[FILE_PATH_MAX];
    file_kind_t kind;
    size_t size;
    const char *mime_type;
} file_info_t;

const char *file_mime_type(const char *path);
file_result_t file_resolve_path(const char *doc_root, const char *request_path, char *resolved, size_t resolved_size);
file_result_t file_stat_path(const char *doc_root, const char *request_path, file_info_t *info);
file_result_t file_build_directory_listing(const char *request_path, const char *resolved_path, char **body, size_t *body_len);

#endif
