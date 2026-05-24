#include "http.h"

#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)
#define ASSERT_STR_EQ(a, b) do { if (strcmp((a), (b)) != 0) { fprintf(stderr, "FAIL: expected '%s' got '%s'\n", (b), (a)); return 1; } } while (0)

static int test_parse_get_http11(void) {
    http_request_t request;
    const char *raw = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(http_parse_request(raw, strlen(raw), &request) == HTTP_PARSE_OK);
    ASSERT_TRUE(request.method == HTTP_METHOD_GET);
    ASSERT_STR_EQ(request.path, "/index.html");
    ASSERT_TRUE(request.version == HTTP_VERSION_11);
    ASSERT_TRUE(http_should_keep_alive(&request));
    return 0;
}

static int test_parse_head_http10_keep_alive(void) {
    http_request_t request;
    const char *raw = "HEAD /about.txt HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    ASSERT_TRUE(http_parse_request(raw, strlen(raw), &request) == HTTP_PARSE_OK);
    ASSERT_TRUE(request.method == HTTP_METHOD_HEAD);
    ASSERT_TRUE(request.version == HTTP_VERSION_10);
    ASSERT_TRUE(http_should_keep_alive(&request));
    return 0;
}

static int test_parse_unsupported_method(void) {
    http_request_t request;
    const char *raw = "POST /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(http_parse_request(raw, strlen(raw), &request) == HTTP_PARSE_OK);
    ASSERT_TRUE(request.method == HTTP_METHOD_UNSUPPORTED);
    return 0;
}

static int test_parse_bad_request(void) {
    http_request_t request;
    const char *raw = "GET /index.html\r\n";
    ASSERT_TRUE(http_parse_request(raw, strlen(raw), &request) == HTTP_PARSE_BAD_REQUEST);
    return 0;
}

static int test_connection_close_disables_keep_alive(void) {
    http_request_t request;
    const char *raw = "GET /index.html HTTP/1.1\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(http_parse_request(raw, strlen(raw), &request) == HTTP_PARSE_OK);
    ASSERT_TRUE(!http_should_keep_alive(&request));
    return 0;
}

int main(void) {
    ASSERT_TRUE(test_parse_get_http11() == 0);
    ASSERT_TRUE(test_parse_head_http10_keep_alive() == 0);
    ASSERT_TRUE(test_parse_unsupported_method() == 0);
    ASSERT_TRUE(test_parse_bad_request() == 0);
    ASSERT_TRUE(test_connection_close_disables_keep_alive() == 0);
    puts("unit_http: PASS");
    return 0;
}
