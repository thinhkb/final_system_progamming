# Level 6 Deep Dive — File System

Level 6 là nơi chúng ta hiểu cách server truy cập file system để phục vụ static files. Đây là tầng vật lý cuối cùng — từ bytes trên đĩa cứng đến response body trên network. Phần quan trọng nhất ở đây là bảo mật: ngăn chặn path traversal attacks.

---

## Mục lục

1. [File System Calls — Tổng quan](#1-file-system-calls--tổng-quan)
2. [Path Resolution — `realpath()`](#2-path-resolution--realpath)
3. [File Metadata — `stat()`](#3-file-metadata--stat)
4. [Directory Listing — `opendir()` / `readdir()`](#4-directory-listing--opendir--readdir)
5. [Path Traversal Attack — Chi tiết và phòng thủ](#5-path-traversal-attack--chi-tiết-và-phòng-thủ)
6. [URL Decoding — `%XX` Encoding](#6-url-decoding--xx-encoding)
7. [HTML Escaping — Chống XSS](#7-html-escaping--chống-xss)
8. [String Builder — Dynamic HTML generation](#8-string-builder--dynamic-html-generation)
9. [Security Flow tổng hợp](#9-security-flow-tổng-hợp)

---

## 1. File System Calls — Tổng quan

### 1.1 Các system calls trong project

```
┌──────────────────────────────────────────────────────────────────┐
│  FILE SYSTEM CALLS TRONG PROJECT                                 │
│                                                                  │
│  realpath(path)     → Resolves symlinks, "..", "." → absolute │
│  stat(path, &st)    → Gets metadata (size, type, permissions)  │
│  fopen(path, "rb")  → Opens file for reading (binary)         │
│  fread(buf, 1, n, f) → Reads n bytes from file               │
│  fseeko(f, off, SEEK_SET) → Seeks to offset (for Range)      │
│  fclose(f)          → Closes file                           │
│  opendir(path)      → Opens directory                       │
│  readdir(dir)       → Reads next directory entry             │
│  closedir(dir)      → Closes directory                      │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 Code flow: Serving a file

```c
// src/server.c:handle_path() → file_stat_path() → send_file_response()

// Bước 1: Resolve path + check security
file_result_t result = file_stat_path(doc_root, request_path, &info);
// → stat() → kiểm tra file/directory
// → Trả FILE_RESULT_OK / NOT_FOUND / FORBIDDEN / ERROR

// Bước 2: Nếu directory → build listing HTML
if (info.kind == FILE_KIND_DIRECTORY) {
    file_build_directory_listing(path, resolved, &body, &len);
}

// Bước 3: Gửi response
send_file_response(fd, request, &info, keep_alive, &response);
```

---

## 2. Path Resolution — `realpath()`

### 2.1 Tại sao cần `realpath()`?

```
Client gửi: GET /../../etc/passwd HTTP/1.1

Nếu không dùng realpath():
  joined = "/www/../../etc/passwd"
  → File system truy cập /etc/passwd → SECURITY BREACH

Với realpath():
  joined = "/www/../../etc/passwd"
  realpath(joined) → "/etc/passwd"  ← Vẫn ra ngoài!
  → path_has_prefix() kiểm tra → FORBIDDEN

Điều gì realpath() làm:
  1. Resolves all symlinks
  2. Resolves all ".." segments
  3. Resolves all "." segments
  4. Returns canonical absolute path
```

### 2.2 `realpath()` trong code

```c
// src/files.c:456–467
char root_real[FILE_PATH_MAX];
char target_real[FILE_PATH_MAX];

/* Bước 3: Phân giải thư mục gốc thành đường dẫn tuyệt đối chuẩn */
if (realpath(doc_root, root_real) == NULL) {
    return FILE_RESULT_ERROR;
}

/* Bước 4: Phân giải đường dẫn file yêu cầu thành tuyệt đối chuẩn */
if (realpath(joined, target_real) == NULL) {
    if (errno == ENOENT || errno == ENOTDIR) {
        return FILE_RESULT_NOT_FOUND;
    }
    return FILE_RESULT_ERROR;
}
```

### 2.3 Ví dụ `realpath()`

```bash
# doc_root = /www
# request_path = /images/../css/style.css

joined = "/www/images/../css/style.css"
realpath("/www") → "/www"  (giả sử không có symlink)

realpath("/www/images/../css/style.css")
  → Resolve ".." → "/www/css/style.css"
  → Return "/www/css/style.css"
  → ✓ Trong /www → ALLOWED

# request_path = /../../etc/passwd
joined = "/www/../../etc/passwd"
realpath("/www/../../etc/passwd")
  → Resolve ".." → "/www/.." → "/"
  → Resolve ".." → "/etc"
  → Resolve ".." → "/etc/passwd"
  → Return "/etc/passwd"
  → ✗ Ngoài /www → FORBIDDEN
```

### 2.4 Symlink resolution

```bash
# doc_root = /www
# www/etc -> /etc (symlink)

realpath("/www/etc") → "/etc"  (symlink resolved!)
# → Ngoài doc_root → FORBIDDEN

# Nhưng nếu symlink nằm TRONG doc_root:
# doc_root = /home/user/public
# public/images/logo.png -> ../images/logo.png (symlink)

realpath("/home/user/public/images/logo.png")
  → "/home/user/images/logo.png"
  → Ngoài doc_root → FORBIDDEN
```

---

## 3. File Metadata — `stat()`

### 3.1 `struct stat` — File metadata

```c
// src/files.c:481–517
struct stat st;

// Lấy metadata của file
if (stat(info->resolved_path, &st) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
        return FILE_RESULT_NOT_FOUND;
    }
    return FILE_RESULT_ERROR;
}

// Kiểm tra type
if (S_ISDIR(st.st_mode)) {
    info->kind = FILE_KIND_DIRECTORY;
    info->size = 0;  // Directory listing tự tạo HTML
    info->mime_type = "text/html";
    return FILE_RESULT_OK;
}

if (S_ISREG(st.st_mode)) {
    info->kind = FILE_KIND_REGULAR;
    info->size = (size_t)st.st_size;  // File size in bytes
    info->mime_type = file_mime_type(info->resolved_path);
    return FILE_RESULT_OK;
}

// Symlinks, sockets, pipes → FORBIDDEN
return FILE_RESULT_FORBIDDEN;
```

### 3.2 `S_ISDIR()` và `S_ISREG()`

```c
// Macro definitions (from sys/stat.h):
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

// S_IFMT = 0170000 (file type mask)
// S_IFDIR = 0040000 (directory)
// S_IFREG = 0100000 (regular file)

/*
File type bits (st_mode & S_IFMT):

  S_IFIFO (0010000) → Named pipe (FIFO)
  S_IFCHR (0020000) → Character device
  S_IFDIR (0040000) → Directory
  S_IFBLK (0060000) → Block device
  S_IFREG (0100000) → Regular file
  S_IFLNK (0120000) → Symbolic link
  S_IFSOCK (0140000) → Socket
*/
```

### 3.3 Permissions — không kiểm tra trong project

```c
// Project KHÔNG kiểm tra permissions
// Ví dụ: nếu file là 000 (no permissions):
//   stat() vẫn thành công
//   fopen(path, "rb") sẽ THẤT BẠI với errno = EACCES

if (fopen(info->resolved_path, "rb") == NULL) {
    return send_simple_response(fd, request, 500, "Internal Server Error\n", 0, result);
}
```

**Điểm hay:** `stat()` thành công ≠ `fopen()` thành công. File có thể readable bởi `stat()` nhưng không readable bởi `fopen()` (ví dụ: execute-only permissions).

---

## 4. Directory Listing — `opendir()` / `readdir()`

### 4.1 Directory reading flow

```c
// src/files.c:519–597
DIR *directory = opendir(resolved_path);
if (directory == NULL) {
    return FILE_RESULT_ERROR;
}

while ((entry = readdir(directory)) != NULL) {
    // entry->d_name = tên file/folder
    // Kiểm tra xem có phải directory không
    stat(child_path, &st);
    if (S_ISDIR(st.st_mode)) {
        is_directory = 1;
    }
    // Thêm vào danh sách entries
    append_directory_entry(&entries, &entry_count, &entry_capacity, ...);
}

closedir(directory);
qsort(entries, entry_count, sizeof(*entries), compare_directory_entries);
```

### 4.2 `struct dirent` — Directory entry

```c
// man dirent
struct dirent {
    ino_t          d_ino;          // Inode number
    off_t          d_off;          // Offset to next entry
    unsigned short d_reclen;       // Length of record
    unsigned char  d_type;         // Type of file (DT_DIR, DT_REG, etc.)
    char           d_name[256];    // Filename (null-terminated)
};
```

### 4.3 Filtering `.` entries

```c
// src/files.c:544–546
if (entry->d_name[0] == '.') {
    continue;  // Bỏ qua . và ..
}
```

**Tại sao bỏ qua `.` và `..`?**
- `.` = current directory (luôn có)
- `..` = parent directory (có thể đi ra ngoài doc_root nếu show)
- Security: Không muốn expose directory structure

### 4.4 Sorting entries

```c
// src/files.c:561
qsort(entries, entry_count, sizeof(*entries), compare_directory_entries);
```

```c
// src/files.c:163–167
static int compare_directory_entries(const void *left, const void *right) {
    const directory_entry_t *left_entry = left;
    const directory_entry_t *right_entry = right;
    return strcmp(left_entry->name, right_entry->name);
}
```

**Sort theo tên (alphabetical)** — `strcmp()` trả về:
- < 0: left < right (left đứng trước)
- = 0: equal
- > 0: left > right (left đứng sau)

---

## 5. Path Traversal Attack — Chi tiết và phòng thủ

### 5.1 Tấn công Path Traversal

```
┌──────────────────────────────────────────────────────────────┐
│  PATH TRAVERSAL ATTACK                                       │
│                                                              │
│  Mục tiêu: Truy cập file NGOÀI doc_root                   │
│                                                              │
│  Ví dụ 1: Basic traversal                                   │
│    Request: GET /../../etc/passwd                            │
│    Doc root: /www                                           │
│    Joined:   /www/../../etc/passwd                          │
│    Resolved: /etc/passwd                                     │
│    → Security breach!                                       │
│                                                              │
│  Ví dụ 2: URL encoded                                       │
│    Request: GET /%2e%2e/%2e%2e/etc/passwd                   │
│    Decoded:  ../.. /etc/passwd                               │
│    → Cần decode trước khi check!                          │
│                                                              │
│  Ví dụ 3: Double encoding                                   │
│    Request: GET /%252e%252e/%252e%252e/etc/passwd          │
│    → %25 = "%" → decode lần 1: %2e%2e/%2e%2e/etc/passwd   │
│    → %2e = "." → decode lần 2: ../../etc/passwd           │
│    → Project này: KHÔNG hỗ trợ double encoding             │
└──────────────────────────────────────────────────────────────┘
```

### 5.2 Phòng thủ Layer 1: Block `..` before `realpath()`

```c
// src/files.c:441–444
if (contains_dot_dot_segment(decoded)) {
    return FILE_RESULT_FORBIDDEN;  // Block ngay lập tức
}
```

```c
// src/files.c:260–275
static int contains_dot_dot_segment(const char *path) {
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return 1;  // Tìm thấy ".." hoặc "../"
        }
        while (*p != '/' && *p != '\0') {
            p++;
        }
    }
    return 0;
}
```

### 5.3 Phòng thủ Layer 2: `path_has_prefix()` sau `realpath()`

```c
// src/files.c:469–472
if (!path_has_prefix(target_real, root_real)) {
    return FILE_RESULT_FORBIDDEN;  // Đã ra ngoài doc_root!
}
```

```c
// src/files.c:277–281
static int path_has_prefix(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0 &&
           (path[prefix_len] == '\0' || path[prefix_len] == '/');
}
```

### 5.4 Tại sao cần 2 layers?

```
Vì Layer 1 có thể bypass được trong một số trường hợp:

Attack 1: Symlink escape
  Doc root: /www
  /www/private -> /home/user/private (symlink!)
  Request: GET /private/../../etc/passwd
  After contains_dot_dot_segment: PASS (không có ".." sau khi decode)
  realpath("/www/private/../../etc/passwd")
    → "/etc/passwd"
  path_has_prefix("/etc/passwd", "/www")
    → FALSE → FORBIDDEN ✓

Attack 2: Case sensitivity (Windows)
  Request: GET /..%2F..%2Fetc/passwd
  decode_path: ".. /.. /etc/passwd"
  contains_dot_dot_segment: TRUE → FORBIDDEN ✓

Attack 3: Null byte injection
  Request: GET /file%00.txt
  decode_path: "file\0.txt"
  ch = '\0' → FILE_RESULT_FORBIDDEN ✓

→ Cả 2 layers cùng cần thiết!
```

### 5.5 Edge cases được xử lý

```c
// src/files.c:277–281
// Case: doc_root = /www, path = /www
path_has_prefix("/www", "/www")
  → prefix_len = 4
  → strncmp("/www", "/www", 4) == 0 → TRUE
  → path[4] == '\0' → TRUE
  → ALLOWED ✓

// Case: doc_root = /www, path = /www/
path_has_prefix("/www/", "/www")
  → prefix_len = 4
  → strncmp("/www/", "/www", 4) == 0 → TRUE
  → path[4] == '/' → TRUE
  → ALLOWED ✓

// Case: doc_root = /www, path = /wwwfoo (không có /)
path_has_prefix("/wwwfoo", "/www")
  → prefix_len = 4
  → strncmp("/wwwfoo", "/www", 4) == 0 → TRUE
  → path[4] == 'f' != '\0' và != '/'
  → FALSE → FORBIDDEN ✓

// Case: doc_root = /www, path = /wwwfile
path_has_prefix("/wwwfile", "/www")
  → prefix_len = 4
  → strncmp("/wwwfile", "/www", 4) == 0 → TRUE
  → path[4] = 'f' != '/' và != '\0'
  → FALSE → FORBIDDEN ✓
```

---

## 6. URL Decoding — `%XX` Encoding

### 6.1 Percent-encoding (URL Encoding)

```
RFC 3986 Section 2.1:
  Characters không an toàn cho URL được mã hóa thành %XX

Ví dụ:
  " " (space) → %20 hoặc +
  "/" → %2F
  "." → %2E
  "%" → %25
  "á" → %C3%A1 (UTF-8 bytes)

Trong HTTP path:
  GET /hello%20world     → file "hello world"
  GET /file%2Ftest      → file "file/test" (URL encoded /)
  GET /100%25off        → file "100%off" (%25 = literal %)
```

### 6.2 `decode_path()` — Decode URL

```c
// src/files.c:220–258
for (size_t i = 0; input[i] != '\0'; i++) {
    unsigned char ch = (unsigned char)input[i];

    if (ch == '%') {
        // Decode %XX
        int high = hex_value(input[i + 1]);  // First hex digit
        int low = hex_value(input[i + 2]);   // Second hex digit
        if (high < 0 || low < 0) {
            return FILE_RESULT_FORBIDDEN;  // Invalid hex → ERROR
        }
        ch = (unsigned char)((high << 4) | low);  // Combine bytes
        i += 2;  // Skip 2 hex digits
    } else if (ch == '+') {
        // "+" trong query string = space
        ch = ' ';
    }

    // Block null byte và backslash
    if (ch == '\0' || ch == '\\') {
        return FILE_RESULT_FORBIDDEN;
    }

    output[written++] = (char)ch;
}
```

### 6.3 `hex_value()` — Convert hex char to int

```c
// src/files.c:207–218
static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';       // '0' → 0, '9' → 9
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;  // 'a' → 10, 'f' → 15
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;  // 'A' → 10, 'F' → 15
    }
    return -1;  // Invalid hex character
}
```

### 6.4 Ví dụ decode

```
Input: "hello%20world"
Step by step:
  'h' → 'h'
  'e' → 'e'
  'l' → 'l'
  'l' → 'l'
  'o' → 'o'
  '%' → decode %20:
           high = hex('2') = 2
           low = hex('0') = 0
           ch = (2 << 4) | 0 = 32 = ' '
  'w' → 'w'
  ...
Output: "hello world"

Input: "file%2Ftest"
Step by step:
  'f' → 'f'
  'i' → 'i'
  'l' → 'l'
  'e' → 'e'
  '%' → decode %2F:
           high = hex('2') = 2
           low = hex('F') = 15
           ch = (2 << 4) | 15 = 47 = '/'
Output: "file/test"
→ Path traversal nếu không check!
```

### 6.5 Null byte attack

```
Input: "file%00.txt"
decode_path:
  'f' → 'f'
  'i' → 'i'
  'l' → 'l'
  'e' → 'e'
  '%' → decode %00:
           ch = '\0'
           ch == '\0' → FILE_RESULT_FORBIDDEN ✓

Nếu không block null byte:
  output = "file\0.txt"
  fopen(output, "rb")
  → fopen("file", "rb") → đọc file sai!
  → SECURITY BREACH
```

---

## 7. HTML Escaping — Chống XSS

### 7.1 XSS (Cross-Site Scripting) Attack

```
┌──────────────────────────────────────────────────────────────┐
│  XSS ATTACK                                                  │
│                                                              │
│  Client gửi request: GET /<script>alert(1)</script>        │
│  Server trả directory listing với filename raw:              │
│    <li><a href="<script>alert(1)</script>">...</a></li>    │
│  Browser của user khác nhìn thấy script chạy!               │
│  → Steal cookies, redirect, deface                         │
└──────────────────────────────────────────────────────────────┘
```

### 7.2 HTML Escaping — 4 ký tự cần escape

```c
// src/files.c:91–122
static int builder_append_html_escaped(string_builder_t *builder, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
            case '&':
                builder_append_text(builder, "&amp;");  // & → &amp;
                break;
            case '<':
                builder_append_text(builder, "&lt;");   // < → &lt;
                break;
            case '>':
                builder_append_text(builder, "&gt;");   // > → &gt;
                break;
            case '"':
                builder_append_text(builder, "&quot;"); // " → &quot;
                break;
            default:
                builder_append_char(builder, *p);
                break;
        }
    }
    return 1;
}
```

### 7.3 Ví dụ escaping

```
Input filename:  <script>alert(1)</script>

Output HTML:
  &lt;script&gt;alert(1)&lt;/script&gt;

→ Browser hiển thị text "<script>..." thay vì execute script

Input filename:  "hello" & 'world'
Output HTML:
  &quot;hello&quot; &amp; &#39;world&#39;
```

### 7.4 Tại sao không escape `'`?

```c
// Trong project KHÔNG escape single quote:
case '"':
    builder_append_text(builder, "&quot;");
    break;
// Case '\'' KHÔNG có

Lý do:
  HTML attribute có thể dùng single quotes:
    <a href='file?name=value'>...</a>
  Nếu filename = 'text' onclick='evil' → XSS
  → Nên escape cả ' hoặc dùng double quotes cho attributes

Trong project:
  <a href="...">name</a>
  → Dùng double quotes → chỉ cần escape "
```

---

## 8. String Builder — Dynamic HTML generation

### 8.1 Dynamic string building problem

```
Nếu dùng fixed buffer:
  char html[1024];
  snprintf(html, sizeof(html), "<li>%s</li>", filename);
  → Overflow nếu filename dài

Nếu dùng char-by-char với realloc:
  str = malloc(1);
  for each char: str = realloc(str, ++len);  // O(n²)!

→ Dùng String Builder với doubling strategy: O(n)
```

### 8.2 String Builder implementation

```c
// src/files.c:11–15
typedef struct {
    char *data;      // Heap buffer
    size_t length;   // Current string length (không tính \0)
    size_t capacity; // Total allocated size
} string_builder_t;
```

### 8.3 Reserve và Grow

```c
// src/files.c:32–66
static int builder_reserve(string_builder_t *builder, size_t additional) {
    size_t required = builder->length + additional + 1;

    if (required <= builder->capacity) {
        return 1;  // Đủ space
    }

    // Doubling strategy
    new_capacity = builder->capacity == 0 ? 1024 : builder->capacity;
    while (new_capacity < required) {
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
```

### 8.4 Growth analysis

```
Initial: capacity = 0
Append 100 bytes:
  required = 0 + 100 + 1 = 101
  capacity = 0 → set to 1024
  1024 >= 101 → OK

Append 1500 bytes (tổng):
  required = 100 + 1500 + 1 = 1601
  capacity = 1024
  1024 < 1601 → grow
  new_capacity = 1024 * 2 = 2048
  2048 >= 1601 → OK

Amortized cost: O(1) per append
Total reallocations: log(n)
```

### 8.5 Append operations

```c
// Append string
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

// Append single character
static int builder_append_char(string_builder_t *builder, char ch) {
    if (!builder_reserve(builder, 1)) {
        return 0;
    }
    builder->data[builder->length++] = ch;
    builder->data[builder->length] = '\0';
    return 1;
}
```

---

## 9. Security Flow tổng hợp

### 9.1 Full security check flow

```
Client Request: GET /images/../%2e%2e/etc/passwd HTTP/1.1

Step 1: Decode URL
  Input:  "/images/../%2e%2e/etc/passwd"
  Output: "/images/../../etc/passwd"
  ✓ Decoded

Step 2: Block ".." segments
  contains_dot_dot_segment("/images/../../etc/passwd")
  → TRUE (tìm thấy "../")
  → FORBIDDEN ✓

---

Client Request: GET /images/logo.png HTTP/1.1

Step 1: Decode URL
  Input:  "/images/logo.png"
  Output: "/images/logo.png"
  ✓ Decoded

Step 2: Block ".." segments
  contains_dot_dot_segment("/images/logo.png")
  → FALSE (không có "..")
  ✓ Passed

Step 3: Join with doc_root
  doc_root: "/www"
  joined: "/www/images/logo.png"

Step 4: Resolve paths
  realpath("/www") → "/www"
  realpath("/www/images/logo.png") → "/www/images/logo.png"
  ✓ Resolved

Step 5: Check prefix
  path_has_prefix("/www/images/logo.png", "/www")
  → TRUE (prefix match)
  ✓ Inside doc_root

Step 6: Stat file
  stat("/www/images/logo.png", &st)
  → S_ISREG(st.st_mode) → TRUE
  ✓ Regular file

Step 7: Open and send
  fopen("/www/images/logo.png", "rb")
  → SUCCESS → Send file
```

### 9.2 Error flow

```
Client Request: GET /nonexistent/file.txt HTTP/1.1

Step 1-5: Security checks → PASS
Step 6: stat() → ENOENT → NOT_FOUND ✓

Client Request: GET /root/.ssh/id_rsa HTTP/1.1
(Bypass doc_root check)

Step 1-5: Security checks → PASS
Step 6: stat() → ENOENT hoặc PERMISSION DENIED
→ NOT_FOUND hoặc ERROR ✓

Client Request: GET /proc/version HTTP/1.1
(Linux /proc filesystem)

Step 1-5: Security checks → PASS
Step 6: stat() → S_ISREG → TRUE
→ Server sẽ đọc và gửi /proc/version
→ ⚠️ Có thể leak system info
→ Fix: Thêm /proc, /sys vào forbidden prefixes
```

---

## Tổng kết Level 6 — Quick Reference

```
┌─────────────────────────────────────────────────────────────┐
│ PATH RESOLUTION                                            │
│                                                             │
│  realpath(doc_root) → root_real                         │
│  realpath(joined) → target_real                         │
│  path_has_prefix(target_real, root_real) → inside?    │
│                                                             │
│  Nếu prefix KHÔNG match → FORBIDDEN                    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ SECURITY LAYERS                                            │
│                                                             │
│  Layer 1: contains_dot_dot_segment()                     │
│    → Block ".." trong decoded path                     │
│                                                             │
│  Layer 2: path_has_prefix() after realpath()            │
│    → Block symlink escapes                             │
│                                                             │
│  Layer 3: S_ISREG/S_ISDIR check                       │
│    → Block symlinks, sockets, devices                 │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ FILE METADATA                                              │
│                                                             │
│  stat(path, &st)                                        │
│  S_ISDIR(st.st_mode) → Directory                        │
│  S_ISREG(st.st_mode) → Regular file                    │
│  st.st_size → File size in bytes                       │
│  S_IFMT masks: DIR, REG, LNK, SOCK, CHR, BLK, FIFO │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ URL DECODING                                              │
│                                                             │
│  %XX → Decode hex to character                         │
│  + → Space (in query string)                           │
│  %00 → FORBIDDEN (null byte attack)                   │
│  \ → FORBIDDEN (backslash attack)                    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ HTML ESCAPING (XSS Prevention)                           │
│                                                             │
│  & → &amp;                                             │
│  < → &lt;                                             │
│  > → &gt;                                             │
│  " → &quot;                                           │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ STRING BUILDER                                            │
│                                                             │
│  Doubling strategy: capacity *= 2 when needed          │
│  Amortized O(1) per append                             │
│  builder_append_text() / builder_append_char()        │
│  builder_destroy() → free(data)                       │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ DIRECTORY LISTING                                         │
│                                                             │
│  opendir() → DIR*                                      │
│  readdir() → struct dirent*                            │
│  Skip "." entries (d_name[0] == '.')                   │
│  qsort() alphabetical                                  │
│  closedir()                                            │
└─────────────────────────────────────────────────────────────┘
```
