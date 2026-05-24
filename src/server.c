#include "server.h"

#include "files.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    server_t *server;
} worker_context_t;

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

static int send_simple_response(int fd, int status, const char *body) {
    char headers[RESPONSE_BUFFER_SIZE];
    size_t body_len = strlen(body);
    int header_len = snprintf(headers, sizeof(headers),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status, http_status_text(status), body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(headers)) {
        return -1;
    }
    return send_all(fd, headers, (size_t)header_len) == 0 &&
           send_all(fd, body, body_len) == 0 ? 0 : -1;
}

static int send_file_response(int fd, const http_request_t *request, const file_info_t *info) {
    FILE *file = fopen(info->resolved_path, "rb");
    char headers[RESPONSE_BUFFER_SIZE];
    char buffer[READ_BUFFER_SIZE];
    size_t remaining = info->size;
    int header_len;

    if (file == NULL) {
        return send_simple_response(fd, 500, "Internal Server Error\n");
    }

    header_len = snprintf(headers, sizeof(headers),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          info->mime_type, info->size);
    if (header_len < 0 || (size_t)header_len >= sizeof(headers) ||
        send_all(fd, headers, (size_t)header_len) != 0) {
        fclose(file);
        return -1;
    }

    if (request->method == HTTP_METHOD_HEAD) {
        fclose(file);
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
    return 0;
}

static int send_directory_response(int fd, const http_request_t *request, const file_info_t *info) {
    char body[RESPONSE_BUFFER_SIZE];
    char headers[RESPONSE_BUFFER_SIZE];
    size_t body_len = 0;
    int header_len;

    if (file_build_directory_listing(request->path, info->resolved_path, body, sizeof(body), &body_len) != FILE_RESULT_OK) {
        return send_simple_response(fd, 500, "Internal Server Error\n");
    }

    header_len = snprintf(headers, sizeof(headers),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(headers) ||
        send_all(fd, headers, (size_t)header_len) != 0) {
        return -1;
    }
    if (request->method == HTTP_METHOD_HEAD) {
        return 0;
    }
    return send_all(fd, body, body_len);
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
    ssize_t received;
    http_request_t request;
    file_info_t info;
    file_result_t file_result;

    received = recv(client_fd, buffer, READ_BUFFER_SIZE, 0);
    if (received <= 0) {
        close(client_fd);
        return;
    }
    buffer[received] = '\0';

    if (http_parse_request(buffer, (size_t)received, &request) != HTTP_PARSE_OK) {
        send_simple_response(client_fd, 400, "Bad Request\n");
        close(client_fd);
        return;
    }

    if (request.method == HTTP_METHOD_UNSUPPORTED) {
        send_simple_response(client_fd, 501, "Not Implemented\n");
        close(client_fd);
        return;
    }

    file_result = file_stat_path(server->config.doc_root, request.path, &info);
    if (file_result != FILE_RESULT_OK) {
        int status = status_from_file_result(file_result);
        char body[128];
        snprintf(body, sizeof(body), "%d %s\n", status, http_status_text(status));
        send_simple_response(client_fd, status, body);
        close(client_fd);
        return;
    }

    if (info.kind == FILE_KIND_DIRECTORY) {
        send_directory_response(client_fd, &request, &info);
    } else {
        send_file_response(client_fd, &request, &info);
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

    server->listen_fd = create_listening_socket(config);
    if (server->listen_fd < 0) {
        socket_queue_destroy(server->queue);
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
    socket_queue_destroy(server->queue);
    server->queue = NULL;
}
