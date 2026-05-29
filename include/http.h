#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_METHOD_TEXT_MAX 16
#define HTTP_PATH_MAX 1024
#define HTTP_VERSION_TEXT_MAX 16
#define HTTP_CONNECTION_TEXT_MAX 32

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_UNSUPPORTED
} http_method_t;

typedef enum {
    HTTP_VERSION_10,
    HTTP_VERSION_11,
    HTTP_VERSION_UNKNOWN
} http_version_t;

typedef enum {
    HTTP_PARSE_OK,
    HTTP_PARSE_BAD_REQUEST
} http_parse_result_t;

typedef struct {
    char method_text[HTTP_METHOD_TEXT_MAX];
    char path[HTTP_PATH_MAX];
    char version_text[HTTP_VERSION_TEXT_MAX];
    char connection[HTTP_CONNECTION_TEXT_MAX];
    http_method_t method;
    http_version_t version;
    int keep_alive_requested;
    int has_range;
    int range_is_suffix;
    int range_end_provided;
    size_t range_start;
    size_t range_end;
} http_request_t;

http_parse_result_t http_parse_request(const char *raw, size_t length, http_request_t *request);
int http_should_keep_alive(const http_request_t *request);
const char *http_status_text(int status_code);

#endif
