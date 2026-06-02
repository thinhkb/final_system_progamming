# Cẩm Nang Chuẩn Bị QA Dự Án: HTTP File Server (C)

Tài liệu này tổng hợp chi tiết giải thích mã nguồn của từng file và những câu hỏi trọng tâm mà giảng viên thường hỏi (kèm theo cách trả lời ghi điểm) để giúp bạn chuẩn bị tốt nhất cho buổi vấn đáp (QA).

---

## 1. Phân Tích Chi Tiết Từng File Code

### 📁 [thread_pool.c](file:///Ubuntu-24.04/home/duc/final_system_progamming/src/thread_pool.c) - Quản lý Luồng & Hàng đợi Socket

*   **Chức năng chính:** Quản lý vòng đời của nhóm luồng làm việc (Worker Threads) và hàng đợi socket (`socket_queue_t`) chứa các kết nối đang chờ xử lý từ client.
*   **Các thành phần quan trọng:**
    *   **Hàng đợi vòng tròn (Circular Queue):** Mảng động `items` lưu trữ danh sách các file descriptor socket (`client_fd`), quản lý qua các con trỏ chỉ số `head` và `tail`.
    *   **Khóa Mutex (`pthread_mutex_t mutex`):** Đảm bảo tại một thời điểm chỉ có duy nhất một luồng được truy cập và sửa đổi các dữ liệu dùng chung của hàng đợi (như biến `count`, `head`, `tail`).
    *   **Biến điều kiện (`pthread_cond_t not_empty` & `not_full`):** 
        *   `not_empty`: Báo hiệu cho các worker threads đang ngủ khi hàng đợi rỗng. Khi có kết nối mới được đưa vào bằng `enqueue`, hàm sẽ phát tín hiệu `pthread_cond_signal(&queue->not_empty)` để đánh thức một luồng dậy xử lý.
        *   `not_full`: Báo hiệu hàng đợi có chỗ trống (tuy nhiên trong thiết kế này, luồng chính không bị block khi hàng đợi đầy mà trả về lỗi trực tiếp).
*   **Các hàm hệ thống POSIX sử dụng:** `pthread_mutex_init`, `pthread_mutex_lock`, `pthread_mutex_unlock`, `pthread_cond_init`, `pthread_cond_wait`, `pthread_cond_signal`, `pthread_cond_broadcast`, `pthread_create`, `pthread_join`.

---

### 📁 [server.c](file:///Ubuntu-24.04/home/duc/final_system_progamming/src/server.c) - Thiết lập Socket Server & Điều phối Client

*   **Chức năng chính:** Khởi tạo cổng kết nối mạng (Socket), lắng nghe và tiếp nhận kết nối (Accept Loop), phân phối socket của client vào hàng đợi và xử lý giao thức HTTP của từng kết nối.
*   **Các thành phần quan trọng:**
    *   **`create_listening_socket`:** Sử dụng hàm `getaddrinfo` để lấy thông tin địa chỉ mà không phụ thuộc vào IPv4 hay IPv6. Sử dụng tùy chọn `SO_REUSEADDR` qua `setsockopt` để tránh lỗi "Address already in use" khi bạn tắt và bật lại server ngay lập tức.
    *   **Vòng lặp `server_run`:** Liên tục gọi hàm hệ thống chặn `accept()` để chờ kết nối. Khi có kết nối mới, nó gọi `socket_queue_enqueue`. 
    *   **Xử lý hàng đợi đầy (Queue Full):** Nếu hàng đợi đầy, server sẽ lập tức gửi mã lỗi `503 Service Unavailable`, ghi log truy cập và đóng kết nối ngay lập tức tại luồng chính để giải phóng tài nguyên.
    *   **Vòng lặp Keep-Alive (`handle_client`):** Đây là hàm callback chạy trên Worker Thread. Nó sử dụng vòng lặp để tiếp tục đọc (`read_next_request`) và xử lý nhiều yêu cầu tiếp theo trên cùng một kết nối mạng nếu client yêu cầu Keep-Alive.
*   **Các hàm hệ thống POSIX sử dụng:** `getaddrinfo`, `socket`, `bind`, `listen`, `accept`, `setsockopt`, `send`, `recv`, `close`, `getpeername`, `getnameinfo`.

---

### 📁 [http.c](file:///Ubuntu-24.04/home/duc/final_system_progamming/src/http.c) - Phân tích cú pháp HTTP (HTTP Parser)

*   **Chức năng chính:** Đọc chuỗi yêu cầu thô (raw HTTP request) từ bộ đệm của socket và bóc tách thành các thông tin cấu trúc có nghĩa.
*   **Các thành phần quan trọng:**
    *   **Phân tích Request Line:** Sử dụng hàm tìm kiếm ký tự xuống dòng `\r\n` (`find_crlf`) để xác định dòng đầu tiên của yêu cầu. Sau đó bóc tách thành: Phương thức (`GET`/`HEAD`), Đường dẫn (`path`), và Phiên bản HTTP (`HTTP/1.0`/`HTTP/1.1`).
    *   **Loại bỏ Query String:** Nhận diện ký tự `?` trong đường dẫn, chỉ giữ lại đường dẫn file thực tế (ví dụ `/index.html?cache=false` sẽ được lọc chỉ còn `/index.html`).
    *   **Đọc Headers:** Phân tích từng dòng header ở dạng `Key: Value`. Đặc biệt quan tâm đến:
        *   `Connection`: Để xác định Keep-Alive (với HTTP/1.1 mặc định là bật trừ khi có `close`; với HTTP/1.0 mặc định là tắt trừ khi có `keep-alive`).
        *   `Range`: Phân tích yêu cầu tải một phần file (ví dụ `bytes=500-1000` hoặc tải 100 byte cuối `bytes=-100`).
*   **Các kỹ thuật an toàn:** Không dùng các hàm không an toàn dễ gây tràn bộ đệm như `strcpy`, thay vào đó tự viết hàm `copy_bounded` (sử dụng `memcpy` có giới hạn kích thước đích và luôn gán ký tự kết thúc chuỗi `\0`).

---

### 📁 [files.c](file:///Ubuntu-24.04/home/duc/final_system_progamming/src/files.c) - Thao tác Hệ thống Tệp tin & An toàn Bảo mật

*   **Chức năng chính:** Ánh xạ đường dẫn yêu cầu HTTP vào file thực tế trên ổ đĩa, xác thực an toàn đường dẫn, tra cứu MIME type và sinh danh sách thư mục tự động.
*   **Các thành phần quan trọng:**
    *   **Phòng chống tấn công Path Traversal:**
        1.  **URL Decoding:** Hàm `decode_path` giải mã các ký tự mã hóa phần trăm (ví dụ `%2E%2E` -> `..`) để lộ đường dẫn thực tế trước khi kiểm tra.
        2.  **Kiểm tra `..`:** Hàm `contains_dot_dot_segment` từ chối ngay lập tức nếu đường dẫn chứa phân đoạn đi ngược thư mục cha (`..`).
        3.  **Sử dụng `realpath`:** Hàm hệ thống `realpath` sẽ phân giải toàn bộ các liên kết tượng trưng (symlinks), dấu chấm `.` và `..` thành một đường dẫn tuyệt đối chuẩn xác (canonicalized path).
        4.  **Kiểm tra tiền tố:** Dùng `path_has_prefix` so sánh đường dẫn tuyệt đối của tài nguyên yêu cầu xem nó có nằm trong thư mục gốc (`doc_root`) hay không. Nếu không, trả về lỗi `403 Forbidden`.
    *   **Liệt kê thư mục (`file_build_directory_listing`):** Sử dụng các hàm POSIX để mở thư mục, duyệt qua các file con, sắp xếp theo tên bằng thuật toán `qsort` và ghép chúng thành một trang HTML.
    *   **Phòng chống XSS (Cross-Site Scripting):** Mọi tên tệp tin trước khi chèn vào trang HTML đều phải đi qua bộ lọc `builder_append_html_escaped` để thay thế các ký tự nguy hiểm (`<` -> `&lt;`, `>` -> `&gt;`).
*   **Các hàm hệ thống POSIX sử dụng:** `stat`, `realpath`, `opendir`, `readdir`, `closedir`.

---

### 📁 [log.c](file:///Ubuntu-24.04/home/duc/final_system_progamming/src/log.c) - Ghi Nhật Ký Truy Cập an toàn đa luồng

*   **Chức năng chính:** Ghi lại mọi lượt truy cập của người dùng theo định dạng chuẩn **Common Log Format** (CLF) của Apache/Nginx.
*   **Các thành phần quan trọng:**
    *   **Ghi log an toàn đa luồng (Thread-safe Logging):** Vì tất cả các Worker Thread đều xử lý client độc lập và ghi log chung vào cùng một tệp tin, ta sử dụng một khóa mutex `pthread_mutex_t mutex` riêng cho module log để tránh việc các dòng ghi đè hoặc xáo trộn chữ vào nhau.
    *   **Sử dụng `localtime_r`:** Đây là phiên bản reentrant (an toàn đa luồng) của hàm lấy thời gian hệ thống `localtime`, nó không sử dụng vùng đệm tĩnh dùng chung của thư viện C để tránh xung đột dữ liệu giữa các luồng.
    *   **Đẩy dữ liệu tức thời (`fflush`):** Sau khi dùng `fprintf` để ghi, máy chủ lập tức gọi `fflush(log->file)` để đẩy dữ liệu xuống ổ cứng ngay, tránh việc dữ liệu bị treo ở bộ nhớ đệm khi máy chủ gặp sự cố bất ngờ.
*   **Các hàm hệ thống POSIX sử dụng:** `localtime_r`, `strftime`, `pthread_mutex_init`, `pthread_mutex_destroy`, `pthread_mutex_lock`, `pthread_mutex_unlock`, `fopen`, `fclose`, `fflush`.

---

### 📁 [main.c](file:///Ubuntu-24.04/home/duc/final_system_progamming/src/main.c) - Khởi chạy & Tắt máy chủ an toàn

*   **Chức năng chính:** Điểm vào chương trình, xử lý tham số dòng lệnh đầu vào, thiết lập bộ lắng nghe tín hiệu để dừng máy chủ một cách mượt mà (Graceful Shutdown).
*   **Các thành phần quan trọng:**
    *   **`getopt`:** Hàm phân tích các tham số dòng lệnh một cách chuyên nghiệp (ví dụ `-p 8080 -t 4`).
    *   **Bắt tín hiệu hệ thống (`signal`):** Lắng nghe các tín hiệu ngắt chương trình `SIGINT` (khi nhấn Ctrl+C) và `SIGTERM` (khi hệ điều hành yêu cầu dừng tiến trình). 
    *   **Graceful Shutdown:** Khi có tín hiệu dừng, hàm `handle_signal` sẽ gọi `server_stop` để:
        1. Đóng socket lắng nghe `listen_fd` để ngắt vòng lặp `accept`.
        2. Kích hoạt cờ `shutdown = 1` trong hàng đợi socket và broadcast để đánh thức tất cả worker thread đang ngủ dậy thoát ra.
        3. Dùng `pthread_join` để đợi tất cả các worker thread kết thúc hoàn toàn công việc của chúng.
        4. Giải phóng các vùng nhớ động, đóng tệp tin log, đảm bảo không có rò rỉ tài nguyên hệ thống (resource leaks).

---

## 2. Các Câu Hỏi QA Giảng Viên Thường Hỏi & Gợi Ý Trả Lời

### ❓ Câu 1: Tại sao em lại sử dụng mô hình Thread Pool kết hợp với Queue thay vì mỗi khi có client kết nối ta lại tạo một Thread mới (`thread-per-connection`)?
> **💡 Trả lời ăn điểm:** 
> *   **Thứ nhất, tiết kiệm chi phí hệ thống:** Việc tạo luồng (thread creation) và hủy luồng (thread destruction) trong hệ điều hành tốn rất nhiều tài nguyên CPU và bộ nhớ (mỗi luồng cần cấp phát vùng nhớ Stack riêng). Việc tạo trước một nhóm luồng cố định (Thread Pool) giúp tái sử dụng các luồng liên tục.
> *   **Thứ hai, kiểm soát tải (Load Control):** Nếu sử dụng mô hình tạo luồng mới cho mỗi kết nối, khi gặp đợt tấn công hoặc lượng truy cập tăng đột biến (ví dụ 10,000 kết nối cùng lúc), hệ thống sẽ cố tạo ra 10,000 luồng. Điều này dẫn đến cạn kiệt tài nguyên hệ thống, CPU mất nhiều thời gian chuyển đổi ngữ cảnh (Context Switching) hơn là xử lý công việc, dẫn đến sập máy chủ. Mô hình Queue giới hạn giúp chúng ta giới hạn số luồng hoạt động tối đa ở mức an toàn cho CPU và xếp hàng những yêu cầu còn lại.

### ❓ Câu 2: Biến điều kiện (`pthread_cond_t`) hoạt động như thế nào trong hàng đợi của dự án? Tại sao lại dùng vòng lặp `while (count == 0)` thay vì lệnh `if` khi gọi `pthread_cond_wait`?
> **💡 Trả lời ăn điểm:** 
> *   Hàm `pthread_cond_wait(&queue->not_empty, &queue->mutex)` hoạt động như sau: Khi hàng đợi rỗng (`count == 0`), luồng gọi hàm này sẽ tự động giải phóng khóa mutex (để luồng khác có thể nạp socket vào) và đi vào trạng thái ngủ (block). Khi có luồng khác gọi `pthread_cond_signal`, luồng đang ngủ sẽ được đánh thức và tự động khóa lại mutex trước khi trả về.
> *   Chúng ta phải sử dụng vòng lặp `while` thay vì `if` vì hai lý do chính:
>     1.  **Hiện tượng đánh thức giả (Spurious Wakeup):** Hệ điều hành đôi khi có thể đánh thức luồng dậy từ trạng thái ngủ mà không hề có tín hiệu signal thực sự nào được phát ra.
>     2.  **Cạnh tranh luồng:** Khi một luồng được đánh thức, trước khi nó kịp khóa lại mutex và chạy tiếp, một luồng khác có thể đã nhảy vào tranh chấp lấy mất socket duy nhất trong hàng đợi làm cho hàng đợi trở lại rỗng. Do đó, việc kiểm tra bằng vòng lặp `while` đảm bảo luồng chỉ đi tiếp khi hàng đợi thực sự có phần tử.

### ❓ Câu 3: Hãy giải thích cách máy chủ của em ngăn chặn lỗi bảo mật Path Traversal (tấn công vượt qua thư mục gốc)?
> **💡 Trả lời ăn điểm:**
> Máy chủ của em thực hiện quy trình kiểm tra 4 lớp cực kỳ nghiêm ngặt:
> 1.  **Giải mã URL:** Trước hết, giải mã toàn bộ Percent-encoding (như `%2e%2e` chuyển về `..`) để hiển thị đường dẫn nguyên bản.
> 2.  **Chặn chuỗi nguy hiểm:** Kiểm tra chuỗi con `..` trực tiếp bằng hàm `contains_dot_dot_segment`. Nếu thấy phân đoạn đường dẫn chứa `..` thì từ chối ngay.
> 3.  **Phân giải đường dẫn tuyệt đối (Canonicalization):** Sử dụng hàm hệ thống `realpath` để biên dịch đường dẫn yêu cầu kết hợp với thư mục gốc thành một đường dẫn tuyệt đối chuẩn duy nhất trên đĩa cứng (loại bỏ mọi symlink hoặc đường dẫn ảo).
> 4.  **Kiểm tra tiền tố (Prefix Check):** So sánh chuỗi đường dẫn tuyệt đối của tệp tin yêu cầu xem nó có bắt đầu bằng đường dẫn tuyệt đối của thư mục gốc (`doc_root`) hay không. Nếu đường dẫn nằm ngoài (ví dụ `/etc/passwd`), tiền tố sẽ không khớp và máy chủ trả về lỗi `403 Forbidden` ngay lập tức.

### ❓ Câu 4: Trong module ghi log (`log.c`), tại sao em lại sử dụng hàm `localtime_r` thay vì `localtime` thông thường?
> **💡 Trả lời ăn điểm:**
> Hàm `localtime` thông thường trả về một con trỏ tới một cấu trúc `struct tm` tĩnh (static buffer) được chia sẻ toàn cục trong thư viện C. Trong môi trường đa luồng (multi-threaded), nếu hai luồng cùng gọi `localtime` cùng một lúc, luồng này sẽ ghi đè lên dữ liệu thời gian của luồng kia, gây ra lỗi sai lệch thời gian (Race Condition).
> Ngược lại, hàm `localtime_r` là phiên bản an toàn đa luồng (reentrant). Nó yêu cầu luồng gọi truyền vào một con trỏ vùng nhớ cục bộ riêng biệt (`&local_time`) nằm trên Stack của luồng đó để lưu trữ kết quả. Nhờ vậy, mỗi luồng tự ghi nhận thời gian vào bộ nhớ riêng của mình, hoàn toàn không xảy ra xung đột.

### ❓ Câu 5: Khi hàng đợi socket đầy (`queue->count == queue->capacity`), luồng chính (Main Thread) xử lý như thế nào? Tại sao không block luồng chính lại để chờ hàng đợi trống?
> **💡 Trả lời ăn điểm:**
> *   Khi hàng đợi đầy, hàm `socket_queue_enqueue` trả về mã lỗi `QUEUE_FULL`. Ngay lập tức, luồng chính sẽ tự mình gửi phản hồi lỗi `503 Service Unavailable` về cho client, ghi log truy cập và đóng kết nối socket đó ngay lập tức.
> *   Chúng ta **không block** luồng chính vì luồng chính chịu trách nhiệm tiếp nhận các kết nối mới từ hệ điều hành qua `accept()`. Nếu block luồng chính khi hàng đợi đầy, máy chủ sẽ ngưng tiếp nhận kết nối mới hoàn toàn từ hệ điều hành, làm các gói tin kết nối TCP bị ứ đọng ở hàng đợi của nhân hệ điều hành (backlog), khiến máy chủ trông giống như đã bị treo hoặc sập đối với các client bên ngoài. Việc từ chối nhanh (fail-fast) bằng lỗi 503 giúp máy chủ giữ được trạng thái phản hồi tốt ngay cả khi quá tải.
