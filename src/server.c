#include "server.h"

#include "files.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    server_t *server;
} worker_context_t;

typedef struct {
    int status_code;
    size_t body_bytes;
} response_result_t;

static int send_all(int fd, const void *buffer, size_t length) {
    const char *data = buffer;
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int extract_request_line(const char *buffer, char *request_line, size_t request_line_size) {
    const char *line_end = strstr(buffer, "\r\n");
    size_t line_len;

    if (buffer == NULL || request_line == NULL || request_line_size == 0) {
        return -1;
    }

    line_len = line_end == NULL ? strlen(buffer) : (size_t)(line_end - buffer);
    if (line_len >= request_line_size) {
        line_len = request_line_size - 1;
    }
    memcpy(request_line, buffer, line_len);
    request_line[line_len] = '\0';
    return 0;
}

static int send_simple_response(int fd, int status, const char *body, int keep_alive, response_result_t *result) {
    char headers[RESPONSE_BUFFER_SIZE];
    size_t body_len = strlen(body);
    int header_len = snprintf(headers, sizeof(headers),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: %s\r\n"
                              "\r\n",
                              status, http_status_text(status), body_len, keep_alive ? "keep-alive" : "close");
    if (header_len < 0 || (size_t)header_len >= sizeof(headers)) {
        return -1;
    }
    if (send_all(fd, headers, (size_t)header_len) != 0 ||
        send_all(fd, body, body_len) != 0) {
        return -1;
    }
    if (result != NULL) {
        result->status_code = status;
        result->body_bytes = body_len;
    }
    return 0;
}

static int send_file_response(int fd, const http_request_t *request, const file_info_t *info, int keep_alive, response_result_t *result) {
    FILE *file = fopen(info->resolved_path, "rb");
    char headers[RESPONSE_BUFFER_SIZE];
    char buffer[READ_BUFFER_SIZE];
    size_t remaining = info->size;
    int header_len;

    if (file == NULL) {
        return send_simple_response(fd, 500, "Internal Server Error\n", 0, result);
    }

    header_len = snprintf(headers, sizeof(headers),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          info->mime_type, info->size, keep_alive ? "keep-alive" : "close");
    if (header_len < 0 || (size_t)header_len >= sizeof(headers) ||
        send_all(fd, headers, (size_t)header_len) != 0) {
        fclose(file);
        return -1;
    }

    if (request->method == HTTP_METHOD_HEAD) {
        fclose(file);
        if (result != NULL) {
            result->status_code = 200;
            result->body_bytes = 0;
        }
        return 0;
    }

    while (remaining > 0) {
        size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t got = fread(buffer, 1, want, file);
        if (got == 0) {
            fclose(file);
            return -1;
        }
        if (send_all(fd, buffer, got) != 0) {
            fclose(file);
            return -1;
        }
        remaining -= got;
    }

    fclose(file);
    if (result != NULL) {
        result->status_code = 200;
        result->body_bytes = info->size;
    }
    return 0;
}

static int send_directory_response(int fd, const http_request_t *request, const file_info_t *info, int keep_alive, response_result_t *result) {
    char body[RESPONSE_BUFFER_SIZE];
    char headers[RESPONSE_BUFFER_SIZE];
    size_t body_len = 0;
    int header_len;

    if (file_build_directory_listing(request->path, info->resolved_path, body, sizeof(body), &body_len) != FILE_RESULT_OK) {
        return send_simple_response(fd, 500, "Internal Server Error\n", 0, result);
    }

    header_len = snprintf(headers, sizeof(headers),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          body_len, keep_alive ? "keep-alive" : "close");
    if (header_len < 0 || (size_t)header_len >= sizeof(headers) ||
        send_all(fd, headers, (size_t)header_len) != 0) {
        return -1;
    }
    if (request->method == HTTP_METHOD_HEAD) {
        if (result != NULL) {
            result->status_code = 200;
            result->body_bytes = 0;
        }
        return 0;
    }
    if (send_all(fd, body, body_len) != 0) {
        return -1;
    }
    if (result != NULL) {
        result->status_code = 200;
        result->body_bytes = body_len;
    }
    return 0;
}

static int status_from_file_result(file_result_t result) {
    switch (result) {
        case FILE_RESULT_NOT_FOUND:
            return 404;
        case FILE_RESULT_FORBIDDEN:
            return 403;
        default:
            return 500;
    }
}

static void handle_client(int client_fd, void *context) {
    worker_context_t *worker_context = context;
    server_t *server = worker_context->server;
    char buffer[READ_BUFFER_SIZE + 1];
    char request_line[256];
    http_request_t request;
    file_info_t info;
    file_result_t file_result;
    int keep_going = 1;

    while (keep_going) {
        ssize_t received = recv(client_fd, buffer, READ_BUFFER_SIZE, 0);
        response_result_t response = {0, 0};

        if (received <= 0) {
            break;
        }
        buffer[received] = '\0';
        extract_request_line(buffer, request_line, sizeof(request_line));

        if (http_parse_request(buffer, (size_t)received, &request) != HTTP_PARSE_OK) {
            send_simple_response(client_fd, 400, "Bad Request\n", 0, &response);
            access_log_write(server->access_log, "127.0.0.1", request_line, response.status_code, response.body_bytes);
            break;
        }

        keep_going = http_should_keep_alive(&request);

        if (request.method == HTTP_METHOD_UNSUPPORTED) {
            send_simple_response(client_fd, 501, "Not Implemented\n", keep_going, &response);
            access_log_write(server->access_log, "127.0.0.1", request_line, response.status_code, response.body_bytes);
            if (!keep_going) {
                break;
            }
            continue;
        }

        file_result = file_stat_path(server->config.doc_root, request.path, &info);
        if (file_result != FILE_RESULT_OK) {
            int status = status_from_file_result(file_result);
            char body[128];
            snprintf(body, sizeof(body), "%d %s\n", status, http_status_text(status));
            send_simple_response(client_fd, status, body, keep_going, &response);
            access_log_write(server->access_log, "127.0.0.1", request_line, response.status_code, response.body_bytes);
            if (!keep_going) {
                break;
            }
            continue;
        }

        if (info.kind == FILE_KIND_DIRECTORY) {
            send_directory_response(client_fd, &request, &info, keep_going, &response);
        } else {
            send_file_response(client_fd, &request, &info, keep_going, &response);
        }
        access_log_write(server->access_log, "127.0.0.1", request_line, response.status_code, response.body_bytes);
    }

    close(client_fd);
}

static int create_listening_socket(const server_config_t *config) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    char port_text[16];
    int listen_fd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port_text, sizeof(port_text), "%d", config->port);

    if (getaddrinfo(config->host, port_text, &hints, &result) != 0) {
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd == -1) {
            continue;
        }
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(listen_fd, SOMAXCONN) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(result);
    return listen_fd;
}

int server_init(server_t *server, const server_config_t *config) {
    if (server == NULL || config == NULL) {
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->config = *config;
    server->listen_fd = -1;

    if (socket_queue_init(&server->queue, (size_t)config->queue_capacity) != 0) {
        return -1;
    }

    if (access_log_open(&server->access_log, config->access_log) != 0) {
        socket_queue_destroy(server->queue);
        server->queue = NULL;
        return -1;
    }

    server->listen_fd = create_listening_socket(config);
    if (server->listen_fd < 0) {
        access_log_close(server->access_log);
        socket_queue_destroy(server->queue);
        server->access_log = NULL;
        server->queue = NULL;
        return -1;
    }

    return 0;
}

int server_run(server_t *server) {
    worker_context_t context;

    if (server == NULL) {
        return 1;
    }

    context.server = server;
    if (thread_pool_start(&server->pool, server->queue, server->config.thread_count, handle_client, &context) != 0) {
        return 1;
    }

    while (!server->should_stop) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server->should_stop) {
                break;
            }
            continue;
        }
        if (socket_queue_enqueue(server->queue, client_fd) != QUEUE_OK) {
            close(client_fd);
        }
    }

    thread_pool_stop(&server->pool);
    return 0;
}

void server_stop(server_t *server) {
    if (server == NULL) {
        return;
    }
    server->should_stop = 1;
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    socket_queue_shutdown(server->queue);
}

void server_destroy(server_t *server) {
    if (server == NULL) {
        return;
    }
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    access_log_close(server->access_log);
    server->access_log = NULL;
    socket_queue_destroy(server->queue);
    server->queue = NULL;
}
