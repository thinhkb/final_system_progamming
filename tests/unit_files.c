#include "files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)
#define ASSERT_STR_EQ(a, b) do { if (strcmp((a), (b)) != 0) { fprintf(stderr, "FAIL: expected '%s' got '%s'\n", (b), (a)); return 1; } } while (0)

static int test_mime_types(void) {
    ASSERT_STR_EQ(file_mime_type("index.html"), "text/html");
    ASSERT_STR_EQ(file_mime_type("style.css"), "text/css");
    ASSERT_STR_EQ(file_mime_type("unknown.bin"), "application/octet-stream");
    return 0;
}

static int test_resolve_index_under_doc_root(void) {
    char resolved[FILE_PATH_MAX];
    ASSERT_TRUE(file_resolve_path("www", "/index.html", resolved, sizeof(resolved)) == FILE_RESULT_OK);
    ASSERT_TRUE(strstr(resolved, "/www/index.html") != NULL);
    return 0;
}

static int test_reject_traversal(void) {
    char resolved[FILE_PATH_MAX];
    ASSERT_TRUE(file_resolve_path("www", "/../etc/passwd", resolved, sizeof(resolved)) == FILE_RESULT_FORBIDDEN);
    ASSERT_TRUE(file_resolve_path("www", "/%2e%2e/etc/passwd", resolved, sizeof(resolved)) == FILE_RESULT_FORBIDDEN);
    return 0;
}

static int test_directory_kind(void) {
    file_info_t info;
    ASSERT_TRUE(file_stat_path("www", "/listing/", &info) == FILE_RESULT_OK);
    ASSERT_TRUE(info.kind == FILE_KIND_DIRECTORY);
    ASSERT_STR_EQ(info.mime_type, "text/html");
    return 0;
}

static int test_directory_listing(void) {
    file_info_t info;
    char *html = NULL;
    size_t written = 0;
    ASSERT_TRUE(file_stat_path("www", "/listing/", &info) == FILE_RESULT_OK);
    ASSERT_TRUE(file_build_directory_listing("/listing/", info.resolved_path, &html, &written) == FILE_RESULT_OK);
    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(strstr(html, "a.txt") != NULL);
    ASSERT_TRUE(strstr(html, "b.txt") != NULL);
    free(html);
    return 0;
}

static int test_directory_listing_url_encodes_links(void) {
    file_info_t info;
    char *html = NULL;
    size_t written = 0;

    ASSERT_TRUE(file_stat_path("www", "/listing/", &info) == FILE_RESULT_OK);
    ASSERT_TRUE(file_build_directory_listing("/listing/", info.resolved_path, &html, &written) == FILE_RESULT_OK);
    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(strstr(html, "href=\"space%20name%20%231.txt\"") != NULL);
    ASSERT_TRUE(strstr(html, "space name #1.txt") != NULL);
    free(html);
    return 0;
}

int main(void) {
    ASSERT_TRUE(test_mime_types() == 0);
    ASSERT_TRUE(test_resolve_index_under_doc_root() == 0);
    ASSERT_TRUE(test_reject_traversal() == 0);
    ASSERT_TRUE(test_directory_kind() == 0);
    ASSERT_TRUE(test_directory_listing() == 0);
    ASSERT_TRUE(test_directory_listing_url_encodes_links() == 0);
    puts("unit_files: PASS");
    return 0;
}
