#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int append_text(char *buffer, size_t buffer_size, size_t *written, const char *text) {
    size_t length = strlen(text);
    if (*written + length >= buffer_size) {
        return 0;
    }
    memcpy(buffer + *written, text, length);
    *written += length;
    buffer[*written] = '\0';
    return 1;
}

static int append_escaped(char *buffer, size_t buffer_size, size_t *written, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
            case '&':
                if (!append_text(buffer, buffer_size, written, "&amp;")) {
                    return 0;
                }
                break;
            case '<':
                if (!append_text(buffer, buffer_size, written, "&lt;")) {
                    return 0;
                }
                break;
            case '>':
                if (!append_text(buffer, buffer_size, written, "&gt;")) {
                    return 0;
                }
                break;
            case '"':
                if (!append_text(buffer, buffer_size, written, "&quot;")) {
                    return 0;
                }
                break;
            default: {
                char one[2] = {*p, '\0'};
                if (!append_text(buffer, buffer_size, written, one)) {
                    return 0;
                }
                break;
            }
        }
    }
    return 1;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static file_result_t decode_path(const char *input, char *output, size_t output_size) {
    size_t written = 0;

    if (input == NULL || output == NULL || output_size == 0) {
        return FILE_RESULT_ERROR;
    }

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (ch == '%') {
            int high = hex_value(input[i + 1]);
            int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) {
                return FILE_RESULT_FORBIDDEN;
            }
            ch = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (ch == '+') {
            ch = ' ';
        }

        if (ch == '\0' || ch == '\\') {
            return FILE_RESULT_FORBIDDEN;
        }

        if (written + 1 >= output_size) {
            return FILE_RESULT_ERROR;
        }
        output[written++] = (char)ch;
    }

    output[written] = '\0';
    return FILE_RESULT_OK;
}

static int contains_dot_dot_segment(const char *path) {
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return 1;
        }
        while (*p != '/' && *p != '\0') {
            p++;
        }
    }
    return 0;
}

static int path_has_prefix(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0 &&
           (path[prefix_len] == '\0' || path[prefix_len] == '/');
}

const char *file_mime_type(const char *path) {
    const char *extension = strrchr(path, '.');
    if (extension == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(extension, ".html") == 0 || strcmp(extension, ".htm") == 0) {
        return "text/html";
    }
    if (strcmp(extension, ".txt") == 0) {
        return "text/plain";
    }
    if (strcmp(extension, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(extension, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(extension, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(extension, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(extension, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

file_result_t file_resolve_path(const char *doc_root, const char *request_path, char *resolved, size_t resolved_size) {
    char decoded[FILE_PATH_MAX];
    char joined[FILE_PATH_MAX];
    char root_real[FILE_PATH_MAX];
    char target_real[FILE_PATH_MAX];
    const char *relative_path;
    file_result_t decode_result;

    if (doc_root == NULL || request_path == NULL || resolved == NULL || resolved_size == 0) {
        return FILE_RESULT_ERROR;
    }

    decode_result = decode_path(request_path, decoded, sizeof(decoded));
    if (decode_result != FILE_RESULT_OK) {
        return decode_result;
    }

    if (contains_dot_dot_segment(decoded)) {
        return FILE_RESULT_FORBIDDEN;
    }

    relative_path = decoded;
    while (*relative_path == '/') {
        relative_path++;
    }

    if (snprintf(joined, sizeof(joined), "%s/%s", doc_root, relative_path) >= (int)sizeof(joined)) {
        return FILE_RESULT_ERROR;
    }

    if (realpath(doc_root, root_real) == NULL) {
        return FILE_RESULT_ERROR;
    }

    if (realpath(joined, target_real) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return FILE_RESULT_NOT_FOUND;
        }
        return FILE_RESULT_ERROR;
    }

    if (!path_has_prefix(target_real, root_real)) {
        return FILE_RESULT_FORBIDDEN;
    }

    if (strlen(target_real) >= resolved_size) {
        return FILE_RESULT_ERROR;
    }
    strcpy(resolved, target_real);
    return FILE_RESULT_OK;
}

file_result_t file_stat_path(const char *doc_root, const char *request_path, file_info_t *info) {
    struct stat st;
    file_result_t result;

    if (info == NULL) {
        return FILE_RESULT_ERROR;
    }

    memset(info, 0, sizeof(*info));
    result = file_resolve_path(doc_root, request_path, info->resolved_path, sizeof(info->resolved_path));
    if (result != FILE_RESULT_OK) {
        return result;
    }

    if (stat(info->resolved_path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return FILE_RESULT_NOT_FOUND;
        }
        return FILE_RESULT_ERROR;
    }

    if (S_ISDIR(st.st_mode)) {
        info->kind = FILE_KIND_DIRECTORY;
        info->size = 0;
        info->mime_type = "text/html";
        return FILE_RESULT_OK;
    }

    if (S_ISREG(st.st_mode)) {
        info->kind = FILE_KIND_REGULAR;
        info->size = (size_t)st.st_size;
        info->mime_type = file_mime_type(info->resolved_path);
        return FILE_RESULT_OK;
    }

    return FILE_RESULT_FORBIDDEN;
}

file_result_t file_build_directory_listing(const char *request_path, const char *resolved_path, char *buffer, size_t buffer_size, size_t *written) {
    DIR *directory;
    struct dirent *entry;
    size_t used = 0;

    if (request_path == NULL || resolved_path == NULL || buffer == NULL || written == NULL || buffer_size == 0) {
        return FILE_RESULT_ERROR;
    }

    directory = opendir(resolved_path);
    if (directory == NULL) {
        return FILE_RESULT_ERROR;
    }

    buffer[0] = '\0';
    if (!append_text(buffer, buffer_size, &used, "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of ") ||
        !append_escaped(buffer, buffer_size, &used, request_path) ||
        !append_text(buffer, buffer_size, &used, "</title></head><body><h1>Index of ") ||
        !append_escaped(buffer, buffer_size, &used, request_path) ||
        !append_text(buffer, buffer_size, &used, "</h1><ul>")) {
        closedir(directory);
        return FILE_RESULT_ERROR;
    }

    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!append_text(buffer, buffer_size, &used, "<li><a href=\"") ||
            !append_escaped(buffer, buffer_size, &used, entry->d_name) ||
            !append_text(buffer, buffer_size, &used, "\">") ||
            !append_escaped(buffer, buffer_size, &used, entry->d_name) ||
            !append_text(buffer, buffer_size, &used, "</a></li>")) {
            closedir(directory);
            return FILE_RESULT_ERROR;
        }
    }

    closedir(directory);

    if (!append_text(buffer, buffer_size, &used, "</ul></body></html>")) {
        return FILE_RESULT_ERROR;
    }

    *written = used;
    return FILE_RESULT_OK;
}
