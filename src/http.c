#include "http.h"

#include <ctype.h>
#include <stdio.h>
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

static int ascii_case_prefix(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text == '\0') {
            return 0;
        }
        if (tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

static void trim_trailing(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[length - 1] = '\0';
        length--;
    }
}

static void parse_connection_header(const char *headers, http_request_t *request) {
    const char *line = headers;
    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);

        if (line_len == 0) {
            break;
        }

        if (ascii_case_prefix(line, "Connection:")) {
            const char *value = skip_spaces(line + strlen("Connection:"));
            copy_bounded(request->connection, sizeof(request->connection), value, line_len - (size_t)(value - line));
            trim_trailing(request->connection);
            return;
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 2;
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
    size_t request_line_len;

    if (raw == NULL || request == NULL || length == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    memset(request, 0, sizeof(*request));

    line_end = strstr(raw, "\r\n");
    if (line_end == NULL || (size_t)(line_end - raw) >= length) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    request_line_len = (size_t)(line_end - raw);
    first_space = memchr(raw, ' ', request_line_len);
    if (first_space == NULL) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    second_space = memchr(first_space + 1, ' ', request_line_len - (size_t)(first_space + 1 - raw));
    if (second_space == NULL || second_space == first_space + 1) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    if ((size_t)(first_space - raw) == 0 || (size_t)(line_end - second_space - 1) == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    copy_bounded(request->method_text, sizeof(request->method_text), raw, (size_t)(first_space - raw));
    copy_bounded(request->path, sizeof(request->path), first_space + 1, (size_t)(second_space - first_space - 1));
    copy_bounded(request->version_text, sizeof(request->version_text), second_space + 1, (size_t)(line_end - second_space - 1));

    request->method = parse_method(request->method_text);
    request->version = parse_version(request->version_text);
    if (request->version == HTTP_VERSION_UNKNOWN) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    parse_connection_header(line_end + 2, request);

    if (request->version == HTTP_VERSION_11) {
        request->keep_alive_requested = !ascii_case_equal(request->connection, "close");
    } else {
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
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
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
