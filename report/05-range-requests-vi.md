# Range Requests — Vietnamese Interview Summary

**Phase:** 05 | **Tóm tắt:** Hỗ trợ HTTP Range header cho phép client yêu cầu một phần của file

---

## Range Header là gì?

HTTP Range header cho phép client yêu cầu **chỉ một phần** của file thay vì toàn bộ. Ví dụ: tải video, resume download, streaming.

```
Range: bytes=7-10
```

---

## 2 Loại Range được hỗ trợ

| Loại | Ví dụ | Ý nghĩa |
|------|-------|---------|
| **Explicit** | `bytes=7-10` | Lấy byte từ 7 đến 10 (4 bytes) |
| **Suffix** | `bytes=-5` | Lấy 5 bytes cuối cùng |

**Lưu ý:** Chỉ hỗ trợ **một range duy nhất**. Nếu có dấu phẩy (multi-range), request bị coi là không hợp lệ.

---

## Luồng xử lý Range

```
Client gửi Range header
        ↓
  [1] PARSE: src/http.c:parse_range_header()
        - Tách start, end từ chuỗi "bytes=7-10"
        - Nhận biết suffix range (bytes=-5)
        ↓
  [2] RESOLVE: src/server.c:resolve_range()
        - start >= file_size → 416 error
        - end > file_size → clamp xuống file_size-1
        ↓
  [3] RESPOND:
        - partial=1 → HTTP 206 + Content-Range header
        - partial=0 → HTTP 200 (toàn bộ file)
        - error=-1  → HTTP 416 Range Not Satisfiable
```

---

## Response 206: Partial Content

Khi range hợp lệ:

```
HTTP/1.1 206 Partial Content
Content-Type: text/html
Content-Length: 4
Content-Range: bytes 7-10/100
Accept-Ranges: bytes
```

**Content-Range:** `bytes start-end/total`
- `start-end`: vị trí byte trong file (inclusive)
- `total`: kích thước **gốc** của file, không phải range length

**Content-Length:** kích thước của range, ví dụ: bytes 7-10 → Content-Length = 4

---

## Response 416: Range Not Satisfiable

Khi `start >= file_size` (yêu cầu byte vượt quá file):

```
HTTP/1.1 416 Range Not Satisfiable
Content-Range: bytes */100
```

Header `Content-Range: bytes */size` cho client biết kích thước thực của file — không phải yêu cầu sai syntax mà là range không thể satisfy được.

---

## Tại sao cần fseeko?

Để đọc **giữa file** (không phải từ đầu):

```c
// Di chuyển con trỏ file đến vị trí start
fseeko(file, resolved.start, SEEK_SET);

// Đọc và gửi từng chunk 8KB
while (remaining > 0) {
    size_t n = fread(buffer, 1, to_read, file);
    send_all(fd, buffer, n);
    remaining -= n;
}
```

**fseeko vs fseek:** `fseeko` dùng offset 64-bit, hỗ trợ file > 2GB trên hệ thống 32-bit. `fseek()` dùng `long` (32-bit) → tràn cho file lớn.

---

## HEAD Request với Range

HEAD request nhận **cùng headers** như GET nhưng **không có body**:

```c
if (request_method == HTTP_METHOD_HEAD) {
    send_response_headers(fd, status_code, ..., resolved.length, ...);
    return; // ← không đọc file, không gửi body
}
```

Range vẫn được resolve để tạo headers đúng (Content-Range, Content-Length), nhưng file không được đọc.

---

## Key Takeaways

1. **Parse → Resolve → Respond** là 3 bước xử lý range tuần tự
2. **206** = range hợp lệ, có `Content-Range` header
3. **416** = range không thỏa mãn được (`start >= file_size`)
4. **Suffix range** `bytes=-5`: nếu file < 5 bytes → trả về toàn bộ file (không lỗi)
5. **fseeko** để đọc giữa file, **send_all** loop để đảm bảo gửi đủ bytes
6. **Accept-Ranges: bytes** gửi trong **tất cả** response thành công (200 và 206) — để client biết server hỗ trợ range

---

## Câu hỏi phỏng vấn hay gặp

**Q: Tại sao 416 dùng `bytes */size` thay vì chỉ `bytes */`?**
A: RFC 9110 yêu cầu `bytes */size` để client biết kích thước thực của file, giúp client tính toán lại range hợp lệ.

**Q: Nếu client gửi `Range: bytes=-9999999` cho file 100 bytes?**
A: Suffix (9999999) > file_size (100) → clamp start=0 → trả về toàn bộ file với HTTP 200 (không phải 416).

**Q: Tại sao fseeko + fread thay vì đọc toàn bộ rồi cắt memory?**
A: Với file lớn (video, ISO), đọc toàn bộ vào memory là impossible. Chunked streaming chỉ đọc phần cần thiết.
