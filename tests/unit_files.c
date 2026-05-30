#include "files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)
#define ASSERT_STR_EQ(a, b) do { if (strcmp((a), (b)) != 0) { fprintf(stderr, "FAIL: expected '%s' got '%s'\n", (b), (a)); return 1; } } while (0)

static int test_mime_types(void) {
    ASSERT_STR_EQ(file_mime_type("index.html"), "text/html");
    ASSERT_STR_EQ(file_mime_type("style.css"), "text/css");
    ASSERT_STR_EQ(file_mime_type("script.js"), "application/javascript");
    ASSERT_STR_EQ(file_mime_type("data.mjs"), "application/javascript");
    ASSERT_STR_EQ(file_mime_type("image.png"), "image/png");
    ASSERT_STR_EQ(file_mime_type("photo.jpg"), "image/jpeg");
    ASSERT_STR_EQ(file_mime_type("photo.jpeg"), "image/jpeg");
    ASSERT_STR_EQ(file_mime_type("icon.gif"), "image/gif");
    ASSERT_STR_EQ(file_mime_type("vector.svg"), "image/svg+xml");
    ASSERT_STR_EQ(file_mime_type("vector.svgz"), "image/svg+xml");
    ASSERT_STR_EQ(file_mime_type("icon.ico"), "image/x-icon");
    ASSERT_STR_EQ(file_mime_type("photo.bmp"), "image/bmp");
    ASSERT_STR_EQ(file_mime_type("photo.webp"), "image/webp");
    ASSERT_STR_EQ(file_mime_type("doc.pdf"), "application/pdf");
    ASSERT_STR_EQ(file_mime_type("archive.zip"), "application/zip");
    ASSERT_STR_EQ(file_mime_type("archive.tar"), "application/x-tar");
    ASSERT_STR_EQ(file_mime_type("archive.gz"), "application/gzip");
    ASSERT_STR_EQ(file_mime_type("archive.7z"), "application/x-7z-compressed");
    ASSERT_STR_EQ(file_mime_type("font.woff"), "font/woff");
    ASSERT_STR_EQ(file_mime_type("font.woff2"), "font/woff2");
    ASSERT_STR_EQ(file_mime_type("font.ttf"), "font/ttf");
    ASSERT_STR_EQ(file_mime_type("font.otf"), "font/otf");
    ASSERT_STR_EQ(file_mime_type("font.eot"), "application/vnd.ms-fontobject");
    ASSERT_STR_EQ(file_mime_type("module.wasm"), "application/wasm");
    ASSERT_STR_EQ(file_mime_type("transform.xslt"), "application/xslt+xml");
    ASSERT_STR_EQ(file_mime_type("video.mp4"), "video/mp4");
    ASSERT_STR_EQ(file_mime_type("video.webm"), "video/webm");
    ASSERT_STR_EQ(file_mime_type("video.ts"), "video/mp2t");
    ASSERT_STR_EQ(file_mime_type("audio.mp3"), "audio/mpeg");
    ASSERT_STR_EQ(file_mime_type("audio.ogg"), "audio/ogg");
    ASSERT_STR_EQ(file_mime_type("audio.wav"), "audio/wav");
    ASSERT_STR_EQ(file_mime_type("doc.doc"), "application/msword");
    ASSERT_STR_EQ(file_mime_type("doc.docx"), "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    ASSERT_STR_EQ(file_mime_type("sheet.xls"), "application/vnd.ms-excel");
    ASSERT_STR_EQ(file_mime_type("sheet.xlsx"), "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    ASSERT_STR_EQ(file_mime_type("slide.ppt"), "application/vnd.ms-powerpoint");
    ASSERT_STR_EQ(file_mime_type("slide.pptx"), "application/vnd.openxmlformats-officedocument.presentationml.presentation");
    ASSERT_STR_EQ(file_mime_type("script.sh"), "application/x-sh");
    ASSERT_STR_EQ(file_mime_type("source.c"), "text/x-c");
    ASSERT_STR_EQ(file_mime_type("header.h"), "text/x-chdr");
    ASSERT_STR_EQ(file_mime_type("readme.md"), "text/markdown");
    ASSERT_STR_EQ(file_mime_type("data.csv"), "text/csv");
    ASSERT_STR_EQ(file_mime_type("config.xml"), "application/xml");
    ASSERT_STR_EQ(file_mime_type("config.yaml"), "text/yaml");
    ASSERT_STR_EQ(file_mime_type("config.yml"), "text/yaml");
    ASSERT_STR_EQ(file_mime_type("unknown.bin"), "application/octet-stream");
    ASSERT_STR_EQ(file_mime_type("noextension"), "application/octet-stream");
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
