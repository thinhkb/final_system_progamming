# File System & Security — Báo Cáo Kỹ Thuật (Tiếng Việt)

## Tổng Quan

Module `src/files.c` xử lý ba nhiệm vụ chính: phân giải đường dẫn an toàn từ request, tra MIME type, và tạo danh sách thư mục dạng HTML. Toàn bộ logic được thiết kế theo nguyên tắc **defense in depth** — nhiều lớp kiểm tra độc lập.

---

## 1. Pipeline Xử Lý Path (5 Bước)

Mỗi request HTTP mang theo `request_path` (ví dụ: `/images/logo.png`). Pipeline phân giải thành đường dẫn tuyệt đối, an toàn, nằm trong document root.

### Bước 1 — Giải mã URL (URL Decoding)

URL được mã hóa percent: `%20` cho khoảng trắng, `%2F` cho `/`. Hàm `decode_path()` giải mã chuỗi:

```c
// src/files.c — decode_path()
void decode_path(char *dest, const char *src) {
    while (*src) {
        if (*src == '%') {
            // Phải có 2 ký tự hex hợp lệ
            if (!isxdigit(src[1]) || !isxdigit(src[2])) {
                *dest = '\0';
                return;          // Sequence không hợp lệ → reject
            }
            *dest++ = (hex_to_int(src[1]) << 4) | hex_to_int(src[2]);
            src += 3;
        } else if (*src == '+') {
            *dest++ = ' ';      // '+' trong query string = khoảng trắng
            src++;
        } else if (*src == '\\') {
            *dest = '\0';
            return;              // Backslash bị reject trên Unix
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
}
```

Điểm quan trọng: bất kỳ `%XY` nào không hợp lệ (XY không phải hex) → reject ngay. Không chấp nhận malformed encoding.

### Bước 2 — Kiểm Tra Dot-Dot (`..`)

Sau khi giải mã, chuỗi path được quét tìm `..` **trước khi gọi filesystem**:

```c
// src/files.c — contains_dot_dot_segment()
bool contains_dot_dot_segment(const char *path) {
    // Strip leading '/'
    // Scan for ".." as complete segment (not inside a filename)
    // Return true if found → reject
}
```

Tại sao kiểm tra trước `realpath`? Vì `realpath` trên một số hệ thống có thể resolve symlink `..` thành path bên ngoài doc_root. Reject ở string-level là lớp phòng thủ độc lập.

### Bước 3 — Nối Path với Document Root

```c
snprintf(resolved, size, "%s/%s", doc_root, stripped_path);
```

Bỏ `/` đầu tiên của request path rồi nối với doc_root. Dùng `snprintf` đảm bảo không tràn buffer.

### Bước 4 — realpath Resolution

Gọi `realpath(3)` trên **cả** doc_root và target path:

```c
// src/files.c — realpath()
char resolved_doc_root[4096];
if (realpath(doc_root, resolved_doc_root) == NULL) return ERROR;
if (realpath(resolved, canonical) == NULL) return ERROR;
```

`realpath` resolve tất cả symlink, `.`, `..` → trả về canonical absolute path.

### Bước 5 — Kiểm Tra Chroot (Security Boundary)

**Đây là lớp bảo mật quan trọng nhất:**

```c
// src/files.c — Chroot verification
if (strncmp(canonical, resolved_doc_root, strlen(resolved_doc_root)) != 0) {
    return FILE_RESULT_FORBIDDEN;   // Path thoát khỏi doc_root
}
```

Canonical path phải **bắt đầu bằng** canonical doc_root. Nếu không → reject với FORBIDDEN.

Kết quả trả về:

| Giá trị                   | Ý nghĩa                                      |
|---------------------------|----------------------------------------------|
| `FILE_RESULT_OK`          | Path hợp lệ, nằm trong doc_root             |
| `FILE_RESULT_NOT_FOUND`   | File/thư mục không tồn tại                  |
| `FILE_RESULT_FORBIDDEN`   | Path thoát khỏi doc_root hoặc chứa `..`    |
| `FILE_RESULT_ERROR`       | Lỗi hệ thống (memory, realpath)             |

---

## 2. MIME Type Lookup

```c
// src/files.c — file_mime_type()
if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
    return "text/html";
if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
    return "image/jpeg";
// ... các extension khác
return "application/octet-stream";  // Mặc định
```

- `strrchr` tìm dấu `.` cuối cùng (`file.tar.gz` → `.gz`)
- `strcasecmp` không phân biệt hoa thường
- Extension không có trong bảng → `application/octet-stream` (browser sẽ download)

---

## 3. Directory Listing — Tại Sao Cần 2 Lớp Encoding?

Khi request là thư mục, server tạo HTML listing. Cần **hai** loại encoding khác nhau:

### HTML Escape cho Display

Tên file hiển thị trong trình duyệt phải escape để tránh XSS:

| Ký tự gốc | Output HTML  |
|-----------|--------------|
| `&`       | `&amp;`      |
| `<`       | `&lt;`       |
| `>`       | `&gt;`       |
| `"`       | `&quot;`     |

Ví dụ: file `<script>alert(1)</script>` → hiển thị dưới dạng text, không chạy JS.

### URL Encode cho Link `<a href>`

Đường dẫn trong thuộc tính `href` phải URL-encode để click hoạt động:

| Ký tự gốc | Output URL  |
|-----------|-------------|
| ` `       | `%20`       |
| `&`       | `%26`       |
| `<`       | `%3C`       |
| `A & B`   | `A%20%26%20B` |

### Ví dụ đầy đủ

File: `A & B <test>`

| Context | Output |
|---------|--------|
| Hiển thị text | `A &amp; B &lt;test&gt;` |
| Link href | `A%20%26%20B%20%3Ctest%3E` |

**Cả hai cần thiết vì chúng phục vụ hai ngữ cảnh hoàn toàn khác nhau.**

---

## 4. Tóm Tắt Bảo Mật

| Mối đe dọa            | Cách xử lý                                   |
|-----------------------|----------------------------------------------|
| Path traversal (`..`) | String-level reject trước realpath           |
| Symlink escape        | realpath() + chroot strncmp                  |
| Malformed URL encoding| Strict %XX validation → reject                |
| XSS trong dir listing | HTML escape trên tất cả tên file hiển thị   |
| Broken URL in links   | URL encode trên tất cả href                  |

---

## Key Takeaways

1. **Defense in depth**: 5 bước kiểm tra độc lập — không có bước nào là đủ một mình
2. **realpath trên cả hai**: cả doc_root và target đều được resolve → không phụ thuộc vào một đầu vào
3. **Hai encoding khác nhau**: HTML escape cho display ≠ URL encode cho links
4. **String builder pattern**: `string_builder_t` tránh O(n²) concatenation khi build listing
5. **`stat()` per entry**: `readdir` có thể trả `DT_UNKNOWN` trên một số filesystem → gọi stat() chắc chắn
