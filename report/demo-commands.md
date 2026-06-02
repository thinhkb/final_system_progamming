# Demo Commands Guide

Hướng dẫn chạy demo cho bài thuyết trình System Programming - HTTP File Server.

## 1. Build Project

```bash
make clean && make
```

## 2. Chạy Server

```bash
./httpd -p 8080 -r www -t 4 -q 64 -l access.log
```

## 3. Demo Các Tính Năng (Terminal mới)

### 3.1. GET Request - Lấy Trang Chủ

```bash
curl http://127.0.0.1:8080/index.html
```

### 3.2. HEAD Request - Lấy Headers

```bash
curl -I http://127.0.0.1:8080/about.txt
```

### 3.3. Directory Listing

```bash
curl http://127.0.0.1:8080/listing/
```

### 3.4. Byte-Range Request

```bash
curl -r 0-31 http://127.0.0.1:8080/about.txt
```

### 3.5. Query Parameter (Cache Bypass)

```bash
curl http://127.0.0.1:8080/index.html?cache=false
```

### 3.6. URL Encoding - File có khoảng trắng và #

```bash
curl http://127.0.0.1:8080/listing/space%20name%20%231.txt
```

## 4. Demo Các Trường Hợp Lỗi

### 4.1. 404 Not Found

```bash
curl -I http://127.0.0.1:8080/missing.txt
```

### 4.2. 403 Forbidden - Path Traversal

```bash
curl -I --path-as-is http://127.0.0.1:8080/../Makefile
```

### 4.3. 501 Not Implemented - Unsupported Method

```bash
curl -X POST -I http://127.0.0.1:8080/index.html
```

### 4.4. 416 Range Not Satisfiable

```bash
curl -I -r 9999-10000 http://127.0.0.1:8080/about.txt
```

## 5. Demo MIME Types

```bash
curl -I http://127.0.0.1:8080/assets/style.css   # text/css
curl -I http://127.0.0.1:8080/test.xml           # application/xml
curl -I http://127.0.0.1:8080/data.csv           # text/csv
curl -I http://127.0.0.1:8080/fonts/sample.woff   # font/woff
curl -I http://127.0.0.1:8080/sample.pdf          # application/pdf
```

## 6. Demo Keep-Alive (HTTP/1.1 Persistent Connection)

```bash
{
    printf 'GET /about.txt HTTP/1.1\r\nHost: localhost\r\n\r\n'
    printf 'GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
} | nc 127.0.0.1 8080
```

## 7. Chạy Tests

### Unit Tests

```bash
make test-unit
```

### Integration Tests

```bash
make test-integration
```

### Tất Cả Tests

```bash
make test
```

## 8. Benchmark - Stress Test

```bash
make bench
```

Hoặc chạy thủ công:

```bash
bash ./bench/bench.sh 127.0.0.1 8080 /index.html 120
```

## 9. Xem Access Log

```bash
cat access.log
```

## Quick Demo Script (Copy-paste tất cả một lần)

```bash
# Chạy server (Terminal 1)
./httpd -p 8080 -r www -t 4 -q 64 -l access.log

# Demo (Terminal 2)
echo "=== GET trang chu ===" && curl http://127.0.0.1:8080/index.html
echo -e "\n=== HEAD request ===" && curl -I http://127.0.0.1:8080/about.txt
echo -e "\n=== Directory listing ===" && curl http://127.0.0.1:8080/listing/
echo -e "\n=== Byte-range ===" && curl -r 0-31 http://127.0.0.1:8080/about.txt
echo -e "\n=== 404 Not Found ===" && curl -I http://127.0.0.1:8080/missing.txt
echo -e "\n=== 403 Forbidden ===" && curl -I --path-as-is http://127.0.0.1:8080/../Makefile
echo -e "\n=== MIME PDF ===" && curl -I http://127.0.0.1:8080/sample.pdf
echo -e "\n=== Access Log ===" && cat access.log
```
