#include "server.h"

#include "files.h"
#include "http.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    server_t *server;
} worker_context_t;

typedef struct {
    int status_code;
    size_t body_bytes;
} response_result_t;

typedef struct {
    size_t start;
    size_t end;
    size_t length;
    int partial;
} byte_range_t;

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

static const char *response_version(const http_request_t *request) {
    if (request != NULL && request->version == HTTP_VERSION_10) {
        return "HTTP/1.0";
    }
    return "HTTP/1.1";
}

static int extract_request_line(const char *buffer, size_t length, char *request_line, size_t request_line_size) {
    const char *line_end = NULL;
    size_t line_len;

    if (buffer == NULL || request_line == NULL || request_line_size == 0) {
        return -1;
    }

    for (size_t i = 0; i + 1 < length; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            line_end = buffer + i;
            break;
        }
    }

    line_len = line_end == NULL ? length : (size_t)(line_end - buffer);
    if (line_len >= request_line_size) {
        line_len = request_line_size - 1;
    }
    memcpy(request_line, buffer, line_len);
    request_line[line_len] = '\0';
    return 0;
}

static int find_header_end(const char *buffer, size_t length, size_t *header_length) {
    if (buffer == NULL || header_length == NULL || length < 4) {
        return 0;
    }

    for (size_t i = 0; i + 3 < length; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            *header_length = i + 4;
            return 1;
        }
    }

    return 0;
}

static int read_next_request(int client_fd, char *buffer, size_t *buffered, http_request_t *request,
                             char *request_line, size_t request_line_size) {
    size_t header_length = 0;

    if (buffer == NULL || buffered == NULL || request == NULL || request_line == NULL) {
        return -1;
    }

    while (1) {
        if (find_header_end(buffer, *buffered, &header_length)) {
            extract_request_line(buffer, header_length, request_line, request_line_size);
            if (http_parse_request(buffer, header_length, request) != HTTP_PARSE_OK) {
                return -1;
            }
            memmove(buffer, buffer + header_length, *buffered - header_length);
            *buffered -= header_length;
            return 1;
        }

        if (*buffered == READ_BUFFER_SIZE) {
            return -1;
        }

        ssize_t received = recv(client_fd, buffer + *buffered, READ_BUFFER_SIZE - *buffered, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return *buffered == 0 ? 0 : -1;
        }
        *buffered += (size_t)received;
    }
}

static void get_client_ip(int client_fd, char *buffer, size_t buffer_size) {
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (getpeername(client_fd, (struct sockaddr *)&peer_addr, &peer_len) == 0 &&
        getnameinfo((struct sockaddr *)&peer_addr, peer_len, buffer, (socklen_t)buffer_size,
                    NULL, 0, NI_NUMERICHOST) == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "-");
}

static int send_simple_response(int fd, const http_request_t *request, int status, const char *body,
                                int keep_alive, response_result_t *result) {
    char headers[RESPONSE_BUFFER_SIZE];
    size_t body_len = strlen(body);
    int send_body = request == NULL || request->method != HTTP_METHOD_HEAD;
    int header_len = snprintf(headers, sizeof(headers),
                              "%s %d %s\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: %s\r\n"
                              "\r\n",
                              response_version(request), status, http_status_text(status), body_len,
                              keep_alive ? "keep-alive" : "close");

    if (header_len < 0 || (size_t)header_len >= sizeof(headers)) {
        return -1;
    }
    if (send_all(fd, headers, (size_t)header_len) != 0) {
        return -1;
    }
    if (send_body && send_all(fd, body, body_len) != 0) {
        return -1;
    }

    if (result != NULL) {
        result->status_code = status;
        result->body_bytes = send_body ? body_len : 0;
    }

    return 0;
}

static int resolve_range(const http_request_t *request, size_t file_size, byte_range_t *range) {
    if (request == NULL || range == NULL) {
        return -1;
    }

    if (!request->has_range) {
        range->start = 0;
        range->end = file_size == 0 ? 0 : file_size - 1;
        range->length = file_size;
        range->partial = 0;
        return 0;
    }

    if (file_size == 0) {
        return -1;
    }

    if (request->range_is_suffix) {
        size_t suffix_length = request->range_start;
        range->start = suffix_length >= file_size ? 0 : file_size - suffix_length;
        range->end = file_size - 1;
    } else {
        if (request->range_start >= file_size) {
            return -1;
        }
        range->start = request->range_start;
        range->end = request->range_end_provided ? request->range_end : (file_size - 1);
        if (range->end < range->start) {
            return -1;
        }
        if (range->end >= file_size) {
            range->end = file_size - 1;
        }
    }

    range->length = range->end - range->start + 1;
    range->partial = 1;
    return 0;
}

static int send_range_not_satisfiable(int fd, const http_request_t *request, size_t file_size,
                                      int keep_alive, response_result_t *result) {
    char headers[RESPONSE_BUFFER_SIZE];
    const char *body = "Range Not Satisfiable\n";
    size_t body_len = strlen(body);
    int send_body = request == NULL || request->method != HTTP_METHOD_HEAD;
    int header_len = snprintf(headers, sizeof(headers),
                              "%s 416 %s\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Range: bytes */%zu\r\n"
                              "Connection: %s\r\n"
                              "\r\n",
                              response_version(request), http_status_text(416), body_len, file_size,
                              keep_alive ? "keep-alive" : "close");

    if (header_len < 0 || (size_t)header_len >= sizeof(headers)) {
        return -1;
    }
    if (send_all(fd, headers, (size_t)header_len) != 0) {
        return -1;
    }
    if (send_body && send_all(fd, body, body_len) != 0) {
        return -1;
    }

    if (result != NULL) {
        result->status_code = 416;
        result->body_bytes = send_body ? body_len : 0;
    }

    return 0;
}

static int send_file_response(int fd, const http_request_t *request, const file_info_t *info,
                              int keep_alive, response_result_t *result) {
    FILE *file = fopen(info->resolved_path, "rb");
    char headers[RESPONSE_BUFFER_SIZE];
    char buffer[READ_BUFFER_SIZE];
    byte_range_t range;
    size_t remaining;
    int header_len;
    int status;

    if (file == NULL) {
        return send_simple_response(fd, request, 500, "Internal Server Error\n", 0, result);
    }

    if (resolve_range(request, info->size, &range) != 0) {
        fclose(file);
        return send_range_not_satisfiable(fd, request, info->size, keep_alive, result);
    }

    if (range.partial && fseeko(file, (off_t)range.start, SEEK_SET) != 0) {
        fclose(file);
        return send_simple_response(fd, request, 500, "Internal Server Error\n", 0, result);
    }

    remaining = range.length;
    status = range.partial ? 206 : 200;
    if (range.partial) {
        header_len = snprintf(headers, sizeof(headers),
                              "%s 206 %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Range: bytes %zu-%zu/%zu\r\n"
                              "Accept-Ranges: bytes\r\n"
                              "Connection: %s\r\n"
                              "\r\n",
                              response_version(request), http_status_text(206), info->mime_type,
                              range.length, range.start, range.end, info->size,
                              keep_alive ? "keep-alive" : "close");
    } else {
        header_len = snprintf(headers, sizeof(headers),
                              "%s 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Accept-Ranges: bytes\r\n"
                              "Connection: %s\r\n"
                              "\r\n",
                              response_version(request), info->mime_type, info->size,
                              keep_alive ? "keep-alive" : "close");
    }

    if (header_len < 0 || (size_t)header_len >= sizeof(headers) ||
        send_all(fd, headers, (size_t)header_len) != 0) {
        fclose(file);
        return -1;
    }

    if (request->method == HTTP_METHOD_HEAD) {
        fclose(file);
        if (result != NULL) {
            result->status_code = status;
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
        result->status_code = status;
        result->body_bytes = range.length;
    }
    return 0;
}

static int send_directory_response(int fd, const http_request_t *request, const file_info_t *info,
                                   int keep_alive, response_result_t *result) {
    char *body = NULL;
    char headers[RESPONSE_BUFFER_SIZE];
    size_t body_len = 0;
    int header_len;

    if (file_build_directory_listing(request->path, info->resolved_path, &body, &body_len) != FILE_RESULT_OK) {
        return send_simple_response(fd, request, 500, "Internal Server Error\n", 0, result);
    }

    header_len = snprintf(headers, sizeof(headers),
                          "%s 200 OK\r\n"
                          "Content-Type: text/html\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          response_version(request), body_len, keep_alive ? "keep-alive" : "close");
    if (header_len < 0 || (size_t)header_len >= sizeof(headers) ||
        send_all(fd, headers, (size_t)header_len) != 0) {
        free(body);
        return -1;
    }
    if (request->method == HTTP_METHOD_HEAD) {
        free(body);
        if (result != NULL) {
            result->status_code = 200;
            result->body_bytes = 0;
        }
        return 0;
    }
    if (send_all(fd, body, body_len) != 0) {
        free(body);
        return -1;
    }
    free(body);
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
    char buffer[READ_BUFFER_SIZE];
    char request_line[256];
    char client_ip[128];
    http_request_t request;
    file_info_t info;
    file_result_t file_result;
    int keep_going = 1;
    size_t buffered = 0;

    /* Lấy địa chỉ IP của Client để phục vụ ghi nhật ký (log) */
    get_client_ip(client_fd, client_ip, sizeof(client_ip));

    /* Vòng lặp hỗ trợ tính năng HTTP Keep-Alive (Xử lý nhiều yêu cầu trên cùng một kết nối) */
    while (keep_going) {
        response_result_t response = {0, 0};
        /* Đọc và phân tích yêu cầu HTTP tiếp theo từ luồng socket */
        int read_result = read_next_request(client_fd, buffer, &buffered, &request, request_line, sizeof(request_line));
        int send_result;

        if (read_result == 0) {
            /* Client chủ động đóng kết nối (EOF) */
            break;
        }
        if (read_result < 0) {
            /* Yêu cầu HTTP không hợp lệ (Bad Request) */
            if (buffered > 0) {
                extract_request_line(buffer, buffered, request_line, sizeof(request_line));
            } else {
                snprintf(request_line, sizeof(request_line), "<invalid request>");
            }
            send_simple_response(client_fd, NULL, 400, "Bad Request\n", 0, &response);
            access_log_write(server->access_log, client_ip, request_line, response.status_code, response.body_bytes);
            break;
        }

        /* Kiểm tra xem request có yêu cầu giữ kết nối (Keep-Alive) hay không */
        keep_going = http_should_keep_alive(&request);

        /* Nếu phương thức HTTP không được hỗ trợ (chỉ hỗ trợ GET và HEAD) */
        if (request.method == HTTP_METHOD_UNSUPPORTED) {
            send_result = send_simple_response(client_fd, &request, 501, "Not Implemented\n", keep_going, &response);
            if (send_result != 0) {
                break;
            }
            access_log_write(server->access_log, client_ip, request_line, response.status_code, response.body_bytes);
            if (!keep_going) {
                break;
            }
            continue;
        }

        /* Định vị đường dẫn tệp tin thực tế trên ổ cứng và kiểm tra an toàn bảo mật */
        file_result = file_stat_path(server->config.doc_root, request.path, &info);
        if (file_result != FILE_RESULT_OK) {
            /* Trả về mã lỗi phù hợp (ví dụ: 404 Not Found hoặc 403 Forbidden) */
            int status = status_from_file_result(file_result);
            char body[128];

            snprintf(body, sizeof(body), "%d %s\n", status, http_status_text(status));
            send_result = send_simple_response(client_fd, &request, status, body, keep_going, &response);
            if (send_result != 0) {
                break;
            }
            access_log_write(server->access_log, client_ip, request_line, response.status_code, response.body_bytes);
            if (!keep_going) {
                break;
            }
            continue;
        }

        /* Phân biệt đối tượng yêu cầu là thư mục hay tệp tin để trả về phản hồi tương ứng */
        if (info.kind == FILE_KIND_DIRECTORY) {
            send_result = send_directory_response(client_fd, &request, &info, keep_going, &response);
        } else {
            send_result = send_file_response(client_fd, &request, &info, keep_going, &response);
        }
        if (send_result != 0) {
            break;
        }
        /* Ghi nhật ký truy cập của client sau khi phản hồi thành công */
        access_log_write(server->access_log, client_ip, request_line, response.status_code, response.body_bytes);
    }

    /* Đóng kết nối TCP socket với client */
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
    hints.ai_family = AF_UNSPEC;      /* Hỗ trợ cả địa chỉ IPv4 và IPv6 */
    hints.ai_socktype = SOCK_STREAM;  /* Sử dụng kết nối dòng truyền tin TCP */
    hints.ai_flags = AI_PASSIVE;      /* Thích hợp cho Socket lắng nghe kết nối đến (Wildcard IP) */
    snprintf(port_text, sizeof(port_text), "%d", config->port);

    /* Biên dịch hostname và port thành cấu trúc thông tin địa chỉ kết nối */
    if (getaddrinfo(config->host, port_text, &hints, &result) != 0) {
        return -1;
    }

    /* Duyệt qua danh sách địa chỉ tìm được để thử bind socket */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd == -1) {
            continue;
        }
        /* Cấu hình SO_REUSEADDR để tránh lỗi bị giữ cổng (Address already in use) khi khởi động lại server gấp */
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        /* Bind socket với địa chỉ cổng và đưa socket vào trạng thái lắng nghe kết nối */
        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(listen_fd, SOMAXCONN) == 0) {
            break; /* Thiết lập socket lắng nghe thành công */
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
    /* Khởi động Thread Pool xử lý kết nối song song */
    if (thread_pool_start(&server->pool, server->queue, server->config.thread_count, handle_client, &context) != 0) {
        return 1;
    }

    /* Vòng lặp Accept Loop chạy trên luồng chính để tiếp nhận các kết nối mới */
    while (!server->should_stop) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        queue_result_t enqueue_result;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue; /* Bị ngắt quãng bởi tín hiệu ngắt hệ thống, thử accept lại */
            }
            if (server->should_stop) {
                break; /* Đã có yêu cầu dừng hệ thống, thoát vòng lặp accept */
            }
            continue;
        }

        /* Đẩy socket client nhận được vào hàng đợi cho các worker thread xử lý */
        enqueue_result = socket_queue_enqueue(server->queue, client_fd);
        if (enqueue_result == QUEUE_FULL) {
            /* Nếu hàng đợi quá tải (đầy), luồng chính tự phản hồi mã 503 và đóng kết nối ngay lập tức */
            response_result_t response = {0, 0};
            char client_ip[128];

            get_client_ip(client_fd, client_ip, sizeof(client_ip));
            send_simple_response(client_fd, NULL, 503, "Service Unavailable\n", 0, &response);
            access_log_write(server->access_log, client_ip, "-", response.status_code, response.body_bytes);
            close(client_fd);
        } else if (enqueue_result != QUEUE_OK) {
            /* Nếu hàng đợi gặp lỗi khác hoặc đã đóng, ngắt kết nối với client */
            close(client_fd);
        }
    }

    /* Đợi tất cả các Worker Thread dừng lại hoàn toàn và giải phóng thread pool */
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
