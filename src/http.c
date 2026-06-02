#include "http.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static void copy_bounded(char *dest, size_t dest_size, const char *src, size_t src_len) {
    size_t copy_len = src_len;
    if (copy_len >= dest_size) {
        copy_len = dest_size - 1;
    }
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static int ascii_case_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int ascii_case_equal_bounded(const char *text, size_t text_len, const char *expected) {
    size_t expected_len = strlen(expected);

    if (text_len != expected_len) {
        return 0;
    }

    for (size_t i = 0; i < text_len; i++) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)expected[i])) {
            return 0;
        }
    }

    return 1;
}

static const char *find_crlf(const char *text, size_t length) {
    if (text == NULL || length < 2) {
        return NULL;
    }

    for (size_t i = 0; i + 1 < length; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') {
            return text + i;
        }
    }

    return NULL;
}

static const char *skip_spaces(const char *text, const char *end) {
    while (text < end && (*text == ' ' || *text == '\t')) {
        text++;
    }
    return text;
}

static const char *trim_trailing_span(const char *start, const char *end) {
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    return end;
}

static int parse_size_token(const char *start, const char *end, size_t *value) {
    size_t parsed = 0;

    if (start == NULL || end == NULL || value == NULL || start >= end) {
        return 0;
    }

    for (const char *cursor = start; cursor < end; cursor++) {
        if (!isdigit((unsigned char)*cursor)) {
            return 0;
        }
        if (parsed > (SIZE_MAX - (size_t)(*cursor - '0')) / 10U) {
            return 0;
        }
        parsed = parsed * 10U + (size_t)(*cursor - '0');
    }

    *value = parsed;
    return 1;
}

static void parse_range_header(const char *value, size_t value_len, http_request_t *request) {
    const char *value_end = trim_trailing_span(value, value + value_len);
    const char *range_spec;
    const char *dash;
    const char *comma;
    size_t parsed = 0;

    if (value == NULL || request == NULL) {
        return;
    }

    value = skip_spaces(value, value_end);
    if ((size_t)(value_end - value) < strlen("bytes=") ||
        !ascii_case_equal_bounded(value, strlen("bytes="), "bytes=")) {
        return;
    }

    range_spec = value + strlen("bytes=");
    if (range_spec >= value_end) {
        return;
    }

    comma = memchr(range_spec, ',', (size_t)(value_end - range_spec));
    if (comma != NULL) {
        return;
    }

    dash = memchr(range_spec, '-', (size_t)(value_end - range_spec));
    if (dash == NULL) {
        return;
    }

    if (dash == range_spec) {
        if (!parse_size_token(dash + 1, value_end, &parsed) || parsed == 0) {
            return;
        }
        request->has_range = 1;
        request->range_is_suffix = 1;
        request->range_start = parsed;
        return;
    }

    if (!parse_size_token(range_spec, dash, &parsed)) {
        return;
    }

    request->has_range = 1;
    request->range_start = parsed;
    if (dash + 1 == value_end) {
        return;
    }

    if (!parse_size_token(dash + 1, value_end, &parsed)) {
        request->has_range = 0;
        return;
    }

    request->range_end_provided = 1;
    request->range_end = parsed;
}

static void parse_headers(const char *headers, size_t headers_len, http_request_t *request) {
    size_t offset = 0;

    while (offset < headers_len) {
        const char *line = headers + offset;
        const char *line_end = find_crlf(line, headers_len - offset);
        const char *colon;
        const char *value;
        const char *value_end;
        size_t line_len;

        if (line_end == NULL) {
            break;
        }

        line_len = (size_t)(line_end - line);
        if (line_len == 0) {
            break;
        }

        colon = memchr(line, ':', line_len);
        if (colon != NULL) {
            value = skip_spaces(colon + 1, line_end);
            value_end = trim_trailing_span(value, line_end);

            if (ascii_case_equal_bounded(line, (size_t)(colon - line), "Connection")) {
                copy_bounded(request->connection, sizeof(request->connection), value, (size_t)(value_end - value));
            } else if (ascii_case_equal_bounded(line, (size_t)(colon - line), "Range")) {
                parse_range_header(value, (size_t)(value_end - value), request);
            }
        }

        offset += line_len + 2;
    }
}

static http_method_t parse_method(const char *method_text) {
    if (strcmp(method_text, "GET") == 0) {
        return HTTP_METHOD_GET;
    }
    if (strcmp(method_text, "HEAD") == 0) {
        return HTTP_METHOD_HEAD;
    }
    return HTTP_METHOD_UNSUPPORTED;
}

static http_version_t parse_version(const char *version_text) {
    if (strcmp(version_text, "HTTP/1.0") == 0) {
        return HTTP_VERSION_10;
    }
    if (strcmp(version_text, "HTTP/1.1") == 0) {
        return HTTP_VERSION_11;
    }
    return HTTP_VERSION_UNKNOWN;
}

http_parse_result_t http_parse_request(const char *raw, size_t length, http_request_t *request) {
    const char *line_end;
    const char *first_space;
    const char *second_space;
    const char *target_start;
    const char *query_start;
    size_t target_len;
    size_t path_len;
    size_t request_line_len;

    if (raw == NULL || request == NULL || length == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    memset(request, 0, sizeof(*request));

    /* Bước 1: Tìm dòng đầu tiên của HTTP Request (kết thúc bằng \r\n) */
    line_end = find_crlf(raw, length);
    if (line_end == NULL || (size_t)(line_end - raw) >= length) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    request_line_len = (size_t)(line_end - raw);
    /* Tìm khoảng trắng thứ nhất phân tách phương thức (Method) và đường dẫn (URI) */
    first_space = memchr(raw, ' ', request_line_len);
    if (first_space == NULL) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    /* Tìm khoảng trắng thứ hai phân tách đường dẫn (URI) và phiên bản HTTP (Version) */
    second_space = memchr(first_space + 1, ' ', request_line_len - (size_t)(first_space + 1 - raw));
    if (second_space == NULL || second_space == first_space + 1) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    if ((size_t)(first_space - raw) == 0 || (size_t)(line_end - second_space - 1) == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    target_start = first_space + 1;
    target_len = (size_t)(second_space - first_space - 1);
    
    /* Phát hiện phần Query String (ví dụ: ?cache=false) để loại bỏ, chỉ giữ lại đường dẫn tệp thực tế */
    query_start = memchr(target_start, '?', target_len);
    path_len = query_start == NULL ? target_len : (size_t)(query_start - target_start);
    if (path_len == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    /* Sao chép an toàn có giới hạn kích thước chuỗi sang struct request */
    copy_bounded(request->method_text, sizeof(request->method_text), raw, (size_t)(first_space - raw));
    copy_bounded(request->path, sizeof(request->path), target_start, path_len);
    copy_bounded(request->version_text, sizeof(request->version_text), second_space + 1, (size_t)(line_end - second_space - 1));

    /* Biên dịch phương thức dạng văn bản thành mã Enum tương ứng (GET, HEAD, UNSUPPORTED) */
    request->method = parse_method(request->method_text);
    request->version = parse_version(request->version_text);
    if (request->version == HTTP_VERSION_UNKNOWN) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    /* Phân tích tiếp các Header ở các dòng tiếp theo (như Connection, Range) */
    parse_headers(line_end + 2, length - request_line_len - 2, request);

    /* Xác định trạng thái Keep-Alive của kết nối dựa trên phiên bản HTTP và header Connection */
    if (request->version == HTTP_VERSION_11) {
        /* HTTP/1.1 mặc định bật Keep-Alive ngoại trừ khi client chỉ định rõ Connection: close */
        request->keep_alive_requested = !ascii_case_equal(request->connection, "close");
    } else {
        /* HTTP/1.0 mặc định tắt Keep-Alive ngoại trừ khi client gửi Connection: keep-alive */
        request->keep_alive_requested = ascii_case_equal(request->connection, "keep-alive");
    }

    return HTTP_PARSE_OK;
}

int http_should_keep_alive(const http_request_t *request) {
    return request != NULL && request->keep_alive_requested;
}

const char *http_status_text(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 416:
            return "Range Not Satisfiable";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        default:
            return "Unknown";
    }
}
