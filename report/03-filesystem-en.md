# File System & Security — Technical Report

## Overview

The file system layer (`src/files.c`) is the gatekeeper between HTTP requests and the server's on-disk resources. It handles three interconnected concerns: resolving a client-requested path into a safe, absolute filesystem path; determining the correct MIME type for the response; and generating directory listings when a client requests a directory. Every decision is scrutinized for correctness because path-handling bugs are among the most common and dangerous vulnerabilities in file servers.

---

## 1. Path Resolution Pipeline

Every incoming request carries a `request_path` (e.g., `/images/logo.png`). The path resolution pipeline converts this into a verified absolute path within the document root. It runs through five sequential stages, each of which can reject the request.

### Stage 1 — URL Decoding

HTTP URLs are percent-encoded. Spaces become `%20`, and `%2F` represents a literal `/` inside a path segment. The first step decodes the raw request path.

```c
// src/files.c — URL decoding loop
void decode_path(char *dest, const char *src) {
    while (*src) {
        if (*src == '%') {
            // Expect two hex digits
            if (!isxdigit(src[1]) || !isxdigit(src[2])) {
                *dest = '\0';
                return;          // Invalid %-sequence → reject
            }
            *dest++ = (hex_to_int(src[1]) << 4) | hex_to_int(src[2]);
            src += 3;
        } else if (*src == '+') {
            *dest++ = ' ';      // '+' in query strings is space
            src++;
        } else if (*src == '\\') {
            *dest = '\0';
            return;              // Backslash rejected on Unix
        } else if (*src == '\0') {
            break;
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
}
```

The decoder is strict by design. Any malformed `%XY` sequence (where `XY` is not two valid hexadecimal digits) causes immediate rejection with an empty string return. This eliminates ambiguous interpretations of broken encodings.

### Stage 2 — Dot-Dot Segment Rejection

After decoding, the path is scanned for `..` segments before any filesystem call is made.

```c
// src/files.c — Dot-dot rejection (lines ~38-58)
bool contains_dot_dot_segment(const char *path) {
    size_t len = strlen(path);
    char normalized[4096];
    // Strip leading '/' for segment-by-segment scanning
    if (len > 0 && path[0] == '/') {
        strncpy(normalized, path + 1, sizeof(normalized) - 1);
    } else {
        strncpy(normalized, path, sizeof(normalized) - 1);
    }
    normalized[sizeof(normalized) - 1] = '\0';

    // Remove trailing '/' if present
    if (len > 0 && normalized[len - 1] == '/') {
        normalized[len - 1] = '\0';
    }

    // Scan for ".." as a complete path segment
    const char *p = normalized;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return true;         // "/../" or trailing "/.."
        }
        p++;
    }
    return false;
}
```

This runs **before** `realpath(3)` because `realpath` on some systems may resolve `..` symlinks, potentially returning a path outside the document root even though the input contained `..`. Rejecting `..` at the string level is a defense-in-depth measure.

### Stage 3 — Path Joining

The decoded, cleaned path is joined with the configured document root.

```c
// src/files.c — Path joining (line ~60-66)
snprintf(resolved, size, "%s/%s", doc_root, stripped_path);
```

The `stripped_path` is the decoded path with its leading `/` removed. The join is performed with `snprintf` to guarantee null-termination and prevent buffer overflow.

### Stage 4 — realpath Resolution

Both the document root and the target path are resolved to their canonical absolute paths using `realpath(3)`.

```c
// src/files.c — realpath resolution (lines ~68-79)
char resolved_doc_root[4096];
if (realpath(doc_root, resolved_doc_root) == NULL) {
    return FILE_RESULT_ERROR;
}
if (realpath(resolved, canonical) == NULL) {
    return FILE_RESULT_ERROR;   // File does not exist or is broken symlink
}
```

`realpath(3)` resolves all symbolic links, removes `.`, and resolves `..` components, returning a canonical path. On error (e.g., the file doesn't exist), it returns `NULL`.

### Stage 5 — Chroot Verification

The final resolved path must be verified to be within the document root. This is the critical security boundary.

```c
// src/files.c — Chroot verification (lines ~81-86)
if (strncmp(canonical, resolved_doc_root, strlen(resolved_doc_root)) != 0) {
    return FILE_RESULT_FORBIDDEN;   // Path escaped doc_root
}
if (canonical[strlen(resolved_doc_root)] != '\0' &&
    canonical[strlen(resolved_doc_root)] != '/') {
    return FILE_RESULT_FORBIDDEN;
}
```

The `strncmp` check ensures the canonical path starts with the document root's canonical path. The second check catches the case where the document root is `/var/www` and an attacker requests `/var/www2` — the first character differs, so it would fail the `strncmp`, but it protects against prefix collisions like `/var/www-extra`.

The function returns one of four values:

| Return Value           | Meaning                                                      |
|------------------------|--------------------------------------------------------------|
| `FILE_RESULT_OK`       | Path is valid, within doc_root, and accessible              |
| `FILE_RESULT_NOT_FOUND`| File or directory does not exist                            |
| `FILE_RESULT_FORBIDDEN`| Path escapes document root or `..` was detected             |
| `FILE_RESULT_ERROR`    | System error (memory allocation, realpath failure)          |

---

## 2. MIME Type Lookup

Once a file's kind and size are determined via `stat(2)`, the server needs a Content-Type header value. The `file_mime_type()` function maps file extensions to IANA MIME types.

```c
// src/files.c — MIME type mapping (lines ~90-108)
const char *file_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext || ext == path + strlen(path) - 1) {
        return "application/octet-stream";
    }
    ext++; // skip the '.'

    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
        return "text/html";
    if (strcasecmp(ext, "txt") == 0)
        return "text/plain";
    if (strcasecmp(ext, "css") == 0)
        return "text/css";
    if (strcasecmp(ext, "js") == 0)
        return "application/javascript";
    if (strcasecmp(ext, "json") == 0)
        return "application/json";
    if (strcasecmp(ext, "png") == 0)
        return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0)
        return "image/gif";
    if (strcasecmp(ext, "svg") == 0)
        return "image/svg+xml";

    return "application/octet-stream";
}
```

Key observations:
- `strrchr` finds the last `.` (important: `/dir/file.tar.gz` → `.gz`, not `.tar`)
- `strcasecmp` makes the lookup case-insensitive (`.JPG` and `.jpg` both work)
- The default is `application/octet-stream` for unknown types, signaling the client to download rather than render

---

## 3. Directory Listing Generation

When the requested path resolves to a directory, the server generates an HTML directory listing. This involves four steps: reading directory entries, sorting them, and building the HTML response.

### Step 1 — Open and Read Directory

```c
// src/files.c — Directory reading (lines ~125-145)
DIR *dir = opendir(canonical);
if (!dir) return NULL;

struct dirent *entry;
while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0) continue; // Skip '.'

    // Build full path for stat() — needed because d_type may be DT_UNKNOWN
    snprintf(full_path, sizeof(full_path), "%s/%s", canonical, entry->d_name);
    if (stat(full_path, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
    }
    // ... collect name, is_dir into entries array
}
closedir(dir);
```

Note: `readdir(3)` returns a `struct dirent*`, but `d_type` may be `DT_UNKNOWN` on some filesystems (e.g., NFS, some network filesystems). The code therefore calls `stat(2)` on each entry to reliably determine its type.

### Step 2 — Sort Entries

```c
// src/files.c — Sorting (lines ~147-160)
qsort(entries, count, sizeof(directory_entry_t), compare_entries);
```

The comparison function sorts directories before files (alphabetically within each group), providing a familiar layout:

```
.html       ← directory
README.txt  ← file
index.html  ← file
```

### Step 3 — HTML Escaping for Display

User-supplied filenames must be HTML-escaped to prevent XSS and rendering issues.

```c
// src/files.c — HTML escaping (lines ~162-180)
static void html_escape(const char *src, char *dest) {
    while (*src) {
        switch (*src) {
            case '&':  strcpy(dest, "&amp;");  dest += 5; break;
            case '<':  strcpy(dest, "&lt;");   dest += 4; break;
            case '>':  strcpy(dest, "&gt;");   dest += 4; break;
            case '"':  strcpy(dest, "&quot;"); dest += 6; break;
            default:   *dest++ = *src;         break;
        }
        src++;
    }
    *dest = '\0';
}
```

A filename like `<script>alert(1)</script>` would be rendered as literal text, not executable JavaScript.

### Step 4 — URL Encoding for Links

The `<a href>` attribute needs URL-encoded links so that clicking a directory navigates correctly.

```c
// src/files.c — URL encoding (lines ~182-200)
static void url_encode(const char *src, char *dest, size_t dest_size) {
    const char *end = src + strlen(src);
    while (src < end) {
        if (isalnum(*src) || *src == '.' || *src == '-' || *src == '_') {
            *dest++ = *src;         // Safe characters — copy as-is
        } else if (*src == ' ') {
            strcpy(dest, "%20");
            dest += 3;
        } else {
            snprintf(dest, dest_size - (dest - dest), "%%%02X", (unsigned char)*src);
            dest += 3;
        }
        src++;
    }
    *dest = '\0';
}
```

**Why both?** HTML escaping and URL encoding serve different purposes:

| Context     | Input Example              | Output                        | Why                          |
|-------------|----------------------------|-------------------------------|------------------------------|
| HTML display| `<script>`                  | `&lt;script&gt;`              | Prevents XSS; renders safely |
| URL in href | `My Documents`              | `My%20Documents`               | Spaces are illegal in URLs   |
| Both        | `A & B <C>` in dir name    | Display: `A &amp; B &lt;C`<br>Link: `A%20%26%20B%20%3CC` | Different encoding contexts |

### Step 5 — Build HTML Response

```c
// src/files.c — String builder (lines ~203-250)
typedef struct { char *data; size_t length; size_t capacity; } string_builder_t;

static void sb_append(string_builder_t *sb, const char *str) {
    size_t needed = sb->length + strlen(str) + 1;
    if (needed > sb->capacity) {
        sb->capacity = needed * 2;
        sb->data = realloc(sb->data, sb->capacity);
    }
    strcpy(sb->data + sb->length, str);
    sb->length += strlen(str);
}
```

The string builder uses a dynamic grow buffer pattern. It doubles capacity on reallocation to achieve amortized O(1) append. The final HTML includes:

- `<title>Index of /path</title>` header
- Each entry as `<a href="encoded_name">escaped_name/</a>` (trailing `/` for directories)
- A parent-directory link when not at doc_root

---

## 4. Security Summary

| Threat                  | Mitigation                                        |
|-------------------------|---------------------------------------------------|
| Path traversal (`..`)   | String-level rejection before realpath            |
| Symlink escape          | realpath() + chroot verification (strncmp)        |
| Invalid URL encoding    | Strict %XX validation — reject malformed sequences |
| Backslash on Unix       | Explicit `\` rejection (no-op on Unix, defensive) |
| XSS in directory names  | HTML escaping on all displayed filenames         |
| Broken URL in links     | URL encoding on all href attributes              |
| Directory traversal via `DT_UNKNOWN` | stat() called per entry      |

---

## Key Implementation Details

- **`file_resolve_path`** — the central function. All path validation flows through it. A single function reduces the attack surface vs. scattered path checks.
- **`file_stat_path`** — wraps resolve + stat in one call. Returns `file_info_t` with kind, size, and resolved path.
- **`string_builder_t`** — avoids O(n²) string concatenation. The directory listing is built in one pass with amortized O(1) appends.
- **`qsort` + `compare_entries`** — provides deterministic, sorted output. Useful for caching and debugging.
- Both the document root and the target path are independently resolved via `realpath`, eliminating reliance on either one alone for the security boundary.
