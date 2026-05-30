#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} string_builder_t;

typedef struct {
    char *name;
    int is_directory;
} directory_entry_t;

static void builder_destroy(string_builder_t *builder) {
    if (builder == NULL) {
        return;
    }
    free(builder->data);
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
}

static int builder_reserve(string_builder_t *builder, size_t additional) {
    size_t required;
    size_t new_capacity;
    char *grown;

    if (builder == NULL) {
        return 0;
    }

    if (additional > ((size_t)-1) - builder->length - 1) {
        return 0;
    }
    required = builder->length + additional + 1;
    if (required <= builder->capacity) {
        return 1;
    }

    new_capacity = builder->capacity == 0 ? 1024 : builder->capacity;
    while (new_capacity < required) {
        if (new_capacity > ((size_t)-1) / 2) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    grown = realloc(builder->data, new_capacity);
    if (grown == NULL) {
        return 0;
    }

    builder->data = grown;
    builder->capacity = new_capacity;
    return 1;
}

static int builder_append_text(string_builder_t *builder, const char *text) {
    size_t length = strlen(text);

    if (!builder_reserve(builder, length)) {
        return 0;
    }

    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 1;
}

static int builder_append_char(string_builder_t *builder, char ch) {
    if (!builder_reserve(builder, 1)) {
        return 0;
    }

    builder->data[builder->length++] = ch;
    builder->data[builder->length] = '\0';
    return 1;
}

static int builder_append_html_escaped(string_builder_t *builder, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
            case '&':
                if (!builder_append_text(builder, "&amp;")) {
                    return 0;
                }
                break;
            case '<':
                if (!builder_append_text(builder, "&lt;")) {
                    return 0;
                }
                break;
            case '>':
                if (!builder_append_text(builder, "&gt;")) {
                    return 0;
                }
                break;
            case '"':
                if (!builder_append_text(builder, "&quot;")) {
                    return 0;
                }
                break;
            default:
                if (!builder_append_char(builder, *p)) {
                    return 0;
                }
                break;
        }
    }
    return 1;
}

static int is_url_unreserved(unsigned char ch) {
    return isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

static int builder_append_url_escaped(string_builder_t *builder, const char *text) {
    static const char hex[] = "0123456789ABCDEF";

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (is_url_unreserved(*p)) {
            if (!builder_append_char(builder, (char)*p)) {
                return 0;
            }
        } else {
            char encoded[4];
            encoded[0] = '%';
            encoded[1] = hex[*p >> 4];
            encoded[2] = hex[*p & 0x0f];
            encoded[3] = '\0';
            if (!builder_append_text(builder, encoded)) {
                return 0;
            }
        }
    }

    return 1;
}

static char *duplicate_string(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1);
    return copy;
}

static int compare_directory_entries(const void *left, const void *right) {
    const directory_entry_t *left_entry = left;
    const directory_entry_t *right_entry = right;
    return strcmp(left_entry->name, right_entry->name);
}

static void free_directory_entries(directory_entry_t *entries, size_t entry_count) {
    if (entries == NULL) {
        return;
    }

    for (size_t i = 0; i < entry_count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

static int append_directory_entry(directory_entry_t **entries, size_t *entry_count, size_t *entry_capacity,
                                  const char *name, int is_directory) {
    directory_entry_t *grown;

    if (*entry_count == *entry_capacity) {
        size_t new_capacity = *entry_capacity == 0 ? 16 : *entry_capacity * 2;
        if (new_capacity < *entry_capacity) {
            return 0;
        }

        grown = realloc(*entries, new_capacity * sizeof(**entries));
        if (grown == NULL) {
            return 0;
        }

        *entries = grown;
        *entry_capacity = new_capacity;
    }

    (*entries)[*entry_count].name = duplicate_string(name);
    if ((*entries)[*entry_count].name == NULL) {
        return 0;
    }
    (*entries)[*entry_count].is_directory = is_directory;
    (*entry_count)++;
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
    if (strcmp(extension, ".csv") == 0) {
        return "text/csv";
    }
    if (strcmp(extension, ".xml") == 0) {
        return "application/xml";
    }
    if (strcmp(extension, ".yaml") == 0 || strcmp(extension, ".yml") == 0) {
        return "text/yaml";
    }
    if (strcmp(extension, ".js") == 0 || strcmp(extension, ".mjs") == 0) {
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
    if (strcmp(extension, ".svgz") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(extension, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcmp(extension, ".bmp") == 0) {
        return "image/bmp";
    }
    if (strcmp(extension, ".webp") == 0) {
        return "image/webp";
    }
    if (strcmp(extension, ".pdf") == 0) {
        return "application/pdf";
    }
    if (strcmp(extension, ".zip") == 0) {
        return "application/zip";
    }
    if (strcmp(extension, ".tar") == 0) {
        return "application/x-tar";
    }
    if (strcmp(extension, ".gz") == 0) {
        return "application/gzip";
    }
    if (strcmp(extension, ".7z") == 0) {
        return "application/x-7z-compressed";
    }
    if (strcmp(extension, ".woff") == 0) {
        return "font/woff";
    }
    if (strcmp(extension, ".woff2") == 0) {
        return "font/woff2";
    }
    if (strcmp(extension, ".ttf") == 0) {
        return "font/ttf";
    }
    if (strcmp(extension, ".otf") == 0) {
        return "font/otf";
    }
    if (strcmp(extension, ".eot") == 0) {
        return "application/vnd.ms-fontobject";
    }
    if (strcmp(extension, ".wasm") == 0) {
        return "application/wasm";
    }
    if (strcmp(extension, ".xslt") == 0) {
        return "application/xslt+xml";
    }
    if (strcmp(extension, ".mp4") == 0) {
        return "video/mp4";
    }
    if (strcmp(extension, ".webm") == 0) {
        return "video/webm";
    }
    if (strcmp(extension, ".ts") == 0) {
        return "video/mp2t";
    }
    if (strcmp(extension, ".mp3") == 0) {
        return "audio/mpeg";
    }
    if (strcmp(extension, ".ogg") == 0) {
        return "audio/ogg";
    }
    if (strcmp(extension, ".wav") == 0) {
        return "audio/wav";
    }
    if (strcmp(extension, ".doc") == 0) {
        return "application/msword";
    }
    if (strcmp(extension, ".docx") == 0) {
        return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    }
    if (strcmp(extension, ".xls") == 0) {
        return "application/vnd.ms-excel";
    }
    if (strcmp(extension, ".xlsx") == 0) {
        return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    }
    if (strcmp(extension, ".ppt") == 0) {
        return "application/vnd.ms-powerpoint";
    }
    if (strcmp(extension, ".pptx") == 0) {
        return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    }
    if (strcmp(extension, ".sh") == 0) {
        return "application/x-sh";
    }
    if (strcmp(extension, ".c") == 0) {
        return "text/x-c";
    }
    if (strcmp(extension, ".h") == 0) {
        return "text/x-chdr";
    }
    if (strcmp(extension, ".md") == 0) {
        return "text/markdown";
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

file_result_t file_build_directory_listing(const char *request_path, const char *resolved_path, char **body, size_t *body_len) {
    DIR *directory;
    struct dirent *entry;
    directory_entry_t *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    string_builder_t builder = {0};

    if (request_path == NULL || resolved_path == NULL || body == NULL || body_len == NULL) {
        return FILE_RESULT_ERROR;
    }

    *body = NULL;
    *body_len = 0;

    directory = opendir(resolved_path);
    if (directory == NULL) {
        return FILE_RESULT_ERROR;
    }

    while ((entry = readdir(directory)) != NULL) {
        char child_path[FILE_PATH_MAX];
        struct stat st;
        int is_directory = 0;

        if (entry->d_name[0] == '.') {
            continue;
        }

        if (snprintf(child_path, sizeof(child_path), "%s/%s", resolved_path, entry->d_name) < (int)sizeof(child_path) &&
            stat(child_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            is_directory = 1;
        }

        if (!append_directory_entry(&entries, &entry_count, &entry_capacity, entry->d_name, is_directory)) {
            closedir(directory);
            free_directory_entries(entries, entry_count);
            return FILE_RESULT_ERROR;
        }
    }

    closedir(directory);
    qsort(entries, entry_count, sizeof(*entries), compare_directory_entries);

    if (!builder_append_text(&builder, "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of ") ||
        !builder_append_html_escaped(&builder, request_path) ||
        !builder_append_text(&builder, "</title></head><body><h1>Index of ") ||
        !builder_append_html_escaped(&builder, request_path) ||
        !builder_append_text(&builder, "</h1><ul>")) {
        free_directory_entries(entries, entry_count);
        builder_destroy(&builder);
        return FILE_RESULT_ERROR;
    }

    for (size_t i = 0; i < entry_count; i++) {
        if (!builder_append_text(&builder, "<li><a href=\"") ||
            !builder_append_url_escaped(&builder, entries[i].name) ||
            (entries[i].is_directory && !builder_append_char(&builder, '/')) ||
            !builder_append_text(&builder, "\">") ||
            !builder_append_html_escaped(&builder, entries[i].name) ||
            (entries[i].is_directory && !builder_append_char(&builder, '/')) ||
            !builder_append_text(&builder, "</a></li>")) {
            free_directory_entries(entries, entry_count);
            builder_destroy(&builder);
            return FILE_RESULT_ERROR;
        }
    }

    free_directory_entries(entries, entry_count);

    if (!builder_append_text(&builder, "</ul></body></html>")) {
        builder_destroy(&builder);
        return FILE_RESULT_ERROR;
    }

    *body = builder.data;
    *body_len = builder.length;
    return FILE_RESULT_OK;
}
