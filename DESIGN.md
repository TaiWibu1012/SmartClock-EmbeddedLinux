# DESIGN.md — Project 2: Smart Weather Alarm Clock

**Học viên:** Lê Phúc Tài | **Ngày nộp:** 13/08/2026

> File này nộp **trước khi mở PR code**. Phần thiết kế tập trung xử lý các điểm cốt lõi: đa mục đích nút nhấn, quản lý 2 màn hình, đồng bộ giờ NTP, quản lý chuyển đổi Wi-Fi Soft AP/Station và cơ chế còi báo thức.

---

## 1. Kiến trúc tổng thể

### Sơ đồ khối hệ thống

```text
+-------------------------------------------------------------------------------------------------------+
|                                           KERNEL SPACE                                                |
|                                                                                                       |
|   +-----------------------+        +-----------------------+        +-----------------------------+   |
|   |   Button Driver       |        |     Buzzer Driver     |        |      I2C Kernel Driver      |   |
|   | (GPIO Ngắt 2 Cạnh)    |        |    (GPIO Output)      |        |      (SSD1306 OLED)         |   |
|   +-----------+-----------+        +-----------^-----------+        +--------------^--------------+   |
+---------------|--------------------------------|-----------------------------------|------------------+
                | `/dev/btn_driver`              | `/dev/buzzer_driver`              | `/dev/i2c-1`
================|================================|================================---|==================
+---------------v--------------------------------|-----------------------------------|------------------+
|                                           USERSPACE                                |                  |
|  +---------------------------------------------------------------------------------|---------------+  |
|  | MAIN PROCESS (Smart Weather Alarm Clock)                                        |               |  |
|  |                                                                                 |               |  |
|  |    +------------------------------------------------------------------------+   |               |  |
|  |    |                   SHARED SYSTEM STATE (Mutex Protected)                |   |               |  |
|  |    | - current_screen (CLOCK / WEATHER)   - alarm_ringing (true / false)    |   |               |  |
|  |    | - net_mode (STATION / SOFT_AP)       - alarm_config (HH:MM)            |   |               |  |
|  |    | - weather_data (Temp, Condition...)  - wifi_creds (SSID, Password)     |   |               |  |
|  |    +---^----------------^-------------------^----------------^--------------+   |               |  |
|  |        |                |                   |                |                  |               |  |
|  |  +-----+------+   +-----+--------+    +-----+--------+   +---+----------+       |               |  |
|  |  | btn_thread |   | clock_thread |    |weather_thread|   |webserver_    |       |               |  |
|  |  |            |   | (timerfd 1s) |    | (HTTP Client)|   |thread Server |       |               |  |
|  |  +-----+------+   +-----+--------+    +-----+--------+   +---+----------+       |               |  |
|  |        |                |                   |                |                  |               |  |
|  |        |                +-------------------+----------------+                  |               |  |
|  |        |                                    |                                   |               |  |
|  |        v                                    v                                   v               |  |
|  |  +-----------+                      +---------------+                  +-----------------+      |  |
|  |  | Network   |                      | Display       |                  |  buzzer_thread  |      |  |
|  |  | Manager   |                      | Controller    |                  +--------+--------+      |  |
|  |  +-----+-----+                      +-------+-------+                           |               |  |
|  +--------|------------------------------------|-----------------------------------|---------------+  |
|           | (fork/exec Non-blocking)           +-----------------------------------+                  |
|           |                                                                                           |
|           v                                    v                                   v                  |
|   +-------------------+               +------------------+                +-----------------------+   |
|   |  System Services  |               | Mock Weather     |                | Smartphone / Laptop   |   |
|   | - hostapd         |               | Server (HTTP)    |                | (Web Browser Client)  |   |
|   | - wpa_supplicant  |               +--------^---------+                +-----------^-----------+   |
|   | - dnsmasq         |                        | (HTTP GET /weather)                  | (HTTP GET/POST|
|   | - systemd-timesyncd                      +--------------------------------------+  Config)        |
|   +-------------------+                                                                               |
|                                                                                                       |
+-------------------------------------------------------------------------------------------------------+
```

### Mô tả luồng hoạt động chính
* **Kernel Layer:** Quản lý phần cứng qua Character Drivers bao gồm nút bấm GPIO ngắt `/dev/btn_driver` (có lọc rung phím debounce), còi `/dev/buzzer_driver` và I2C `/dev/i2c-1` cho màn hình OLED SSD1306.
* **Shared State Center:** Tiến trình chính duy trì cấu trúc dữ liệu chung (Mutex Protected) lưu trữ trạng thái màn hình (`CLOCK`/`WEATHER`), cờ báo thức (`alarm_ringing`), cờ mạng và cấu hình báo thức.
* **Luồng xử lý sự kiện & hiển thị:**
  * `btn_thread`: Đọc sự kiện ngắt thô kèm timestamp, phân loại ngắn/dài theo ngưỡng thời gian và ngữ cảnh hệ thống.
  * `clock_thread`: Dùng `timerfd` 1s đồng bộ giờ từ `time()` (tự thích ứng khi NTP đồng bộ làm nhảy giờ), kiểm tra giờ báo thức và vẽ OLED.
  * `weather_thread`: Thức dậy lập tức khi chuyển sang màn hình thời tiết hoặc định kỳ 5 phút bằng `timerfd`. Socket timeout 5s giúp không chặn giao diện khi lỗi mạng.
  * `webserver_thread`: Cung cấp giao diện HTTP cục bộ cấu hình Wi-Fi (Soft AP) và giờ báo thức.
  * `buzzer_thread`: Điều khiển bật/tắt còi dựa trên cờ `alarm_ringing`.
* **Luồng quản lý mạng:** Sử dụng `fork()` + `execvp()` không chặn (non-blocking) tương tác với `hostapd`, `wpa_supplicant`, `dnsmasq` có cơ chế đếm ngược 20s tự khôi phục Soft AP nếu nhập sai password Wi-Fi.

---

## 2. Danh sách Process/Thread

| Tên | Loại (process/thread) | Vai trò | Tạo lúc nào | Kết thúc lúc nào | Giao tiếp với ai — qua kênh gì |
|---|---|---|---|---|---|
| **main** | process | Khởi tạo Mutex, shared state, mở file driver, tạo worker threads và lắng nghe signal dọn dẹp. | Khởi động chương trình. | Khi nhận signal dừng (SIGINT/SIGTERM) hoặc lỗi fatal. | Quản lý vòng đời tất cả các thread. |
| **btn_thread** | thread | Đọc sự kiện thô từ `/dev/btn_driver`, phân loại ngữ cảnh: tắt còi, chuyển màn hình, Soft AP. | Khi `main` khởi tạo xong driver và mutex. | Chạy suốt vòng đời chương trình (vòng lặp vô hạn). | - **Driver:** đọc blocking `/dev/btn_driver`<br>- **Network:** `fork()`/`execvp()` Soft AP<br>- **Shared State:** Cập nhật state (Mutex) |
| **clock_thread** | thread | Dùng `timerfd` định kỳ 1s thức dậy, đọc `time()`, so sánh giờ báo thức và vẽ giao diện Đồng hồ lên OLED. | Khi `main` khởi tạo lúc startup. | Chạy suốt vòng đời chương trình. | - **I2C Driver:** Ghi ra `/dev/i2c-1`<br>- **Shared State:** Đọc/Ghi state (Mutex) |
| **weather_thread** | thread | HTTP Client. Ở màn hình thời tiết, gửi GET request tới Mock Weather Server (khi chuyển vào hoặc mỗi 5 phút). | Khi `main` khởi tạo lúc startup. | Chạy suốt vòng đời chương trình. | - **Mock Server:** TCP Socket port 80/8080<br>- **Shared State:** Đọc/Ghi state (Mutex) |
| **webserver_thread** | thread | HTTP Server (port 8080). Nhận POST form Wi-Fi (Soft AP) hoặc POST giờ báo thức (Station), lưu vào `alarm.conf`. | Khi `main` khởi tạo lúc startup. | Chạy suốt vòng đời chương trình. | - **Client HTTP:** Socket TCP<br>- **Network:** Gọi script kết nối Station<br>- **Shared State:** Ghi state (Mutex) |
| **buzzer_thread** | thread | Đọc cờ `alarm_ringing`, nếu `true` thì bật còi `/dev/buzzer_driver`, nếu `false` thì tắt. | Khi `main` khởi tạo lúc startup. | Chạy suốt vòng đời chương trình. | - **Buzzer Driver:** Ghi tín hiệu ra `/dev/buzzer_driver`<br>- **Shared State:** Đọc `alarm_ringing` (Mutex) |

### Chi tiết các vùng dữ liệu dùng chung (Shared Data Matrix)
* **`current_screen`**: `btn_thread` (Ghi) $\leftrightarrow$ `clock_thread`, `weather_thread` (Đọc)
* **`alarm_ringing`**: `clock_thread`, `btn_thread` (Ghi) $\leftrightarrow$ `buzzer_thread`, `btn_thread` (Đọc)
* **`alarm_config`**: `webserver_thread` (Ghi) $\leftrightarrow$ `clock_thread` (Đọc)
* **`weather_data`**: `weather_thread` (Ghi) $\leftrightarrow$ `clock_thread`/OLED Display (Đọc)
* **`net_mode`**: `btn_thread`, `webserver_thread` (Ghi) $\leftrightarrow$ `clock_thread`, `webserver_thread` (Đọc)

---

## 3. Resource Mapping — GPIO / I2C

### Bảng cấu hình phần cứng

| Thiết bị | GPIO Pin (Physical Pin) | Cách khai báo | Ghi chú |
|---|---|---|---|
| **Nút nhấn (đa chức năng)** | GPIO 17 (Pin 11) | Device Tree Overlay | Cấu hình `active-low`, `pull-up` nội bộ, bắt ngắt 2 cạnh (`IRQ_TYPE_EDGE_BOTH`), kèm bộ lọc chống rung (Debounce). |
| **Buzzer** | GPIO 27 (Pin 13) | Device Tree Overlay | Cấu hình GPIO Output (`active-high`), xuất tín hiệu High/Low điều khiển còi. |
| **OLED SSD1306 (128x64)** | SDA: GPIO 2 (Pin 3)<br>SCL: GPIO 3 (Pin 5) | Bus `/dev/i2c-1`<br>Địa chỉ I2C: `0x3C` | Kích hoạt `i2c1` trong Device Tree. Điều khiển trực tiếp từ Userspace qua Linux I2C Subsystem (`I2C_SLAVE` ioctl). |

### Chi tiết kỹ thuật Button Driver (Debounce & Timestamp)
* **Cơ chế ngắt:** Driver bắt cả 2 cạnh lên và xuống (`IRQ_TYPE_EDGE_BOTH`).
* **Lọc rung phím (Software Debounce):** Khi ngắt xảy ra, driver kiểm tra khoảng cách thời gian với ngắt trước đó. Nếu $< 20\text{ ms}$, bỏ qua sự kiện ngắt (coi là nhiễu rung cơ khí).
* **Mốc thời gian (Timestamp):** Sử dụng `ktime_get_ns()` trong Kernel thu thập mốc thời gian chính xác tính bằng nanosecond ($\mu\text{s}$ precision) và truyền về Userspace qua cấu trúc:
  ```c
  struct button_event {
      uint8_t  state;        // 1: PRESSED, 0: RELEASED
      uint64_t timestamp_ns; // Mốc thời gian ngắt từ ktime_get_ns()
  };
  ```

### Chi tiết cơ chế điều khiển OLED SSD1306 qua I2C ioctl
1. **Khởi tạo:** `int fd = open("/dev/i2c-1", O_RDWR); ioctl(fd, I2C_SLAVE, 0x3C);`
2. **Gửi lệnh (Command Mode - `0x00`):** Gửi chuỗi byte `[0x00, CMD_BYTE]` để cài đặt chế độ hiển thị.
3. **Gửi dữ liệu khung hình (Data Mode - `0x40`):** Duy trì Framebuffer 1024 bytes ($128 \times 64 / 8$). Cập nhật giao diện bằng lệnh `write()` 1025 bytes (1 byte header `0x40` + 1024 bytes data) trong 1 giao dịch I2C duy nhất.

---

## 4. Logic phân loại sự kiện nút nhấn (quan trọng nhất của project)

### Ngưỡng thời gian bấm (Time Thresholds)
Thời gian nhấn ($t = \text{timestamp\_release} - \text{timestamp\_press}$):
* $t < 50\text{ ms}$: Bỏ qua (coi là nhiễu tín hiệu).
* $50\text{ ms} \le t < 5000\text{ ms}$: **Nhấn ngắn (Short Press)**.
* $t \ge 5000\text{ ms}$: **Nhấn giữ (Long Press)**.

### Bảng ma trận ưu tiên xử lý theo ngữ cảnh

| Trạng thái chuông | Thời gian bấm | Kết quả xử lý | Mức ưu tiên |
|---|---|---|---|
| **Buzzer đang kêu** (`alarm_ringing == true`) | Nhấn ngắn ($< 5\text{s}$) | **Tắt còi báo thức ngay lập tức** (`alarm_ringing = false`). Giữ nguyên màn hình hiện tại. Lần nhấn tiếp theo trở lại luồng thường. | **Ưu tiên 1 (Cao nhất)** |
| **Buzzer không kêu** (`alarm_ringing == false`) | Nhấn ngắn ($< 5\text{s}$) | **Chuyển màn hình hiển thị** (`CLOCK` $\leftrightarrow$ `WEATHER`). | **Ưu tiên 2** |
| **Bất kỳ lúc nào** (Buzzer kêu hoặc không) | Nhấn giữ ($\ge 5\text{s}$) | **Vào Smart Config (Soft AP)**. *Trường hợp đặc biệt:* Nếu còi đang kêu, hệ thống sẽ **tắt còi trước**, sau đó chuyển `net_mode = SOFT_AP`. | **Ưu tiên 0 (Ngắt ngầm)** |

### Luồng thuật toán xử lý trong `btn_thread` (Userspace)

```text
[Đọc struct button_event từ /dev/btn_driver]
                      |
                      v
             Có phải sự kiện RELEASED?
             /                       \
           (No)                     (Yes)
            /                         \
  [Bỏ qua hoặc lưu           Tính duration = (timestamp_release - timestamp_press)
  timestamp_press]                     |
                                       v
                             duration >= 5000 ms?
                             /                  \
                           (Yes)                (No)
                            /                     \
             [EVENT_LONG_PRESS_5S]            [EVENT_SHORT_PRESS]
                      |                                   |
                      v                                   v
             Tắt còi (nếu đang kêu)         Khóa Mutex hệ thống (system_state_mutex)
             Chuyển sang Soft AP                          |
             (Kích hoạt hostapd)                Is alarm_ringing == true?
                                                /                      \
                                              (Yes)                    (No)
                                               /                         \
                                     Tắt còi báo thức              Đổi màn hình
                                   (alarm_ringing = false)     (CLOCK <-> WEATHER)
                                               \                         /
                                                \                       /
                                              Giải phóng Mutex hệ thống 
```

---

## 5. Thiết kế 2 màn hình hiển thị

### A. Màn hình đồng hồ (`clock_screen`)
* **Cơ chế đếm giây chính xác:** Sử dụng `timerfd_create(CLOCK_MONOTONIC, 0)` với chu kỳ `it_interval = {1, 0}` định kỳ 1s thức dậy.
* **Chống trôi giờ & Xử lý NTP Time Jumps:** 
  * Không dùng `sleep(1)` cộng dồn biến đếm. Tại mỗi nhịp tick của `timerfd`, chương trình gọi `time(NULL)` lấy giờ thực hệ thống và phân tích `localtime()` vẽ chuỗi `HH:MM:SS`.
  * **Xử lý khi NTP đồng bộ nhảy giờ:** Khi `systemd-timesyncd` đồng bộ giờ thành công làm giờ hệ thống nhảy đột ngột (ví dụ từ 07:59:50 sang 08:05:30), `timerfd` (chạy trên đồng hồ đơn điệu `CLOCK_MONOTONIC`) vẫn duy trì nhịp tick 1s ổn định không bị đứng/trễ. Hàm `time(NULL)` lập tức trả về giá trị giờ mới đã đồng bộ và vẽ thẳng ra OLED ở nhịp tick kế tiếp.

### B. Màn hình thời tiết (`weather_screen`)
* **Cơ chế Refresh 5 phút:** 
  * Sử dụng `timerfd` chu kỳ 300 giây.
  * **Xử lý chuyển màn hình tức thì:** Khi người dùng nhấn nút chuyển từ `CLOCK` sang `WEATHER`, `btn_thread` bật cờ `force_weather_fetch = true`. `weather_thread` đọc được cờ này sẽ gửi HTTP request lấy dữ liệu mới lập tức mà không phải chờ hết chu kỳ 5 phút.
* **Cơ chế Non-blocking & Xử lý lỗi hiển thị OLED:**
  * Toàn bộ thao tác mạng cô lập trong `weather_thread`. Thiết lập Socket Timeout `SO_RCVTIMEO` = 5s.
  * **Giao diện OLED khi lỗi/timeout mạng:** 
    * Giữ nguyên dữ liệu thời tiết cũ thu được gần nhất, vẽ thêm icon mất mạng nhỏ `[!] Offline` ở góc trên bên phải màn hình.
    * Nếu chưa từng lấy được dữ liệu thành công lần nào mà sập mạng $\rightarrow$ Hiển thị chuỗi: `"Weather: N/A (Net Error)"`.

---

## 6. Thiết kế Webserver & Quản lý Mạng (Network Management)

### A. Kiến trúc Webserver
* **Mô hình xử lý:** Dùng `accept()` lặp đơn luồng trên port 8080 xử lý tuần tự từng HTTP Request.
* **Phân tích Request thô:** Bóc tách dòng đầu tiên lấy `METHOD` (`GET`/`POST`) và `PATH`, parse body dạng `application/x-www-form-urlencoded`.

### B. Danh sách Endpoints

| Endpoint | Method | Chế độ mạng | Chức năng | Dữ liệu nhận vào (Body) | Phản hồi (Response) |
|---|---|---|---|---|---|
| `/` | `GET` | Soft AP / Station | Trả về HTML Form nhập Wi-Fi (Soft AP) hoặc Form đặt giờ báo thức (Station). | *(Không)* | `200 OK` + HTML Form embedded. |
| `/wifi-config` | `POST` | Soft AP | Nhận SSID và Password Wi-Fi nhà. | `ssid=MyHomeWiFi&password=12345678` | `200 OK` + Trang báo đang kết nối. |
| `/alarm-config` | `POST` | Station | Nhận giờ/phút báo thức mới và ghi đè. | `hour=07&minute=30` | `200 OK` + Trang báo lưu thành công. |

### C. Cơ chế chuyển đổi Soft AP ↔ Station Mode & Fallback
Mọi lệnh quản lý mạng gọi qua `fork()` + `execvp()` từ tiến trình con chuyên trách để tránh chặn (block) `btn_thread`:
1. **Chuyển Station $\rightarrow$ Soft AP (Nhấn giữ 5s):**
   * Tắt Station: `systemctl stop wpa_supplicant@wlan0`
   * Gán IP tĩnh: `ip addr add 192.168.4.1/24 dev wlan0`
   * Bật Soft AP: `systemctl start hostapd` và `systemctl start dnsmasq`
2. **Chuyển Soft AP $\rightarrow$ Station (Submit Form Wi-Fi):**
   * Dừng Soft AP: `systemctl stop hostapd` và `systemctl stop dnsmasq`
   * Ghi file `/etc/wpa_supplicant/wpa_supplicant.conf`
   * Bật Station: `systemctl restart wpa_supplicant@wlan0` và xin IP bằng `udhcpc`
3. **Cơ chế Fallback khi sai Password Wi-Fi:**
   * Tiến trình con thực hiện kiểm tra trạng thái `wpa_cli status` trong vòng **20 giây**.
   * Nếu sau 20 giây không đạt trạng thái `wpa_state=COMPLETED` $\rightarrow$ Coi như sai mật khẩu. Hệ thống tự động hủy Station và kích hoạt lại luồng Soft AP để người dùng nhập lại Wi-Fi trên web.

### D. Mã mẫu Ghi đè file cấu hình an toàn (Atomic File Write Pattern)
Để đảm bảo file `alarm.conf` không bị hư hỏng (corrupt) nếu mất điện giữa chừng:

```c
#include <stdio.h>
#include <unistd.h>

void save_alarm_config_atomic(int hour, int minute, int enabled) {
    // 1. Mở file tạm để ghi
    FILE *fp = fopen("/etc/smartclock/alarm.conf.tmp", "w");
    if (!fp) return;

    // 2. Ghi nội dung cấu hình mới
    fprintf(fp, "hour=%d\nminute=%d\nenabled=%d\n", hour, minute, enabled);

    // 3. Ép bộ đệm ghi hoàn toàn xuống đĩa cứng vật lý
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    // 4. Đổi tên file nguyên tử (Atomic Rename)
    rename("/etc/smartclock/alarm.conf.tmp", "/etc/smartclock/alarm.conf");
}
```

---

## 7. Đồng bộ hoá & Race Condition

### A. Cơ chế bảo vệ vùng nhớ dùng chung (Shared Memory)
Sử dụng một Mutex duy nhất bảo vệ toàn bộ `Shared System State`:
```c
pthread_mutex_t system_state_mutex = PTHREAD_MUTEX_INITIALIZER;
```
* **Lý do:** Loại bỏ 100% rủi ro Deadlock, đơn giản hóa luồng code, chi phí lock/unlock trên POSIX cực thấp.

### B. Bảng phân tích nguy cơ Race Condition và Giải pháp

| Dữ liệu dùng chung | Các Thread đụng vào | Nguy cơ Race Condition | Cơ chế giải quyết (Mutex Boundary) |
|---|---|---|---|
| **`alarm_ringing`**<br>*(Boolean)* | - **Write:** `clock_thread` (set `true`), `btn_thread` (set `false`).<br>- **Read:** `btn_thread`, `buzzer_thread`. | `clock_thread` vừa bật còi đúng lúc `btn_thread` tắt còi, dẫn tới sai lệch trạng thái hiển thị và còi kêu không dừng. | Bọc toàn bộ thao tác đọc/ghi `alarm_ringing` trong `pthread_mutex_lock()` / `pthread_mutex_unlock()`. |
| **`alarm_config`**<br>*(Struct: hour, minute, enabled)* | - **Write:** `webserver_thread`.<br>- **Read:** `clock_thread`. | Struct gồm nhiều byte. Nếu `clock_thread` đọc giữa chừng khi `webserver_thread` mới ghi được 1 phần, giờ báo thức sẽ bị rác. | `webserver_thread` chỉ được ghi đè struct sau khi lấy Mutex. `clock_thread` đọc nguyên bản struct ra local buffer rồi mới nhả Mutex. |
| **`weather_data`**<br>*(Struct: temp, condition[32], last_update)* | - **Write:** `weather_thread`.<br>- **Read:** `clock_thread` (khi vẽ OLED). | Mảng chuỗi `condition` gồm nhiều byte. Đọc không đồng bộ gây vỡ chuỗi hiển thị lên OLED. | `weather_thread` xin Mutex để `memcpy` dữ liệu. `clock_thread` xin Mutex copy ra local trước khi vẽ. |
| **`current_screen`**<br>*(Enum: CLOCK/WEATHER)* | - **Write:** `btn_thread`.<br>- **Read:** `clock_thread`, `weather_thread`. | Chuyển đổi màn hình liên tục khi nhấn nút nhanh, khiến `weather_thread` kích hoạt lấy dữ liệu sai thời điểm. | Kiểm tra và cập nhật enum màn hình atomic dưới Mutex. |
| **`net_mode`**<br>*(Enum: STATION/SOFT_AP)* | - **Write:** `btn_thread`, `webserver_thread`.<br>- **Read:** `clock_thread`, `webserver_thread`. | Đổi chế độ mạng đồng thời từ cả nút bấm và web interface. | Tất cả chuyển đổi trạng thái mạng phải thông qua Mutex bảo vệ cờ `net_mode`. |

---

## 8. Edge Case / Failure Handling dự kiến

| Tình huống | Cách xử lý dự kiến |
|---|---|
| **Mất kết nối khi đang ở màn hình thời tiết** | Socket timeout 5s. Nếu fetch thất bại, giữ dữ liệu cũ + hiện icon `[!] Offline` góc OLED. Không block giao diện, không crash. |
| **Đến giờ báo thức đúng lúc đang ở giữa Smart Config** | `clock_thread` so sánh giờ độc lập. Khi tới giờ, bật `alarm_ringing = true` kích còi. Nhấn ngắn để tắt còi mà không mất trạng thái Soft AP. |
| **Nhấn giữ nút (≥5s) trong lúc buzzer đang kêu** | Tắt còi báo thức ngay lập tức, đồng thời chuyển hệ thống sang chế độ Smart Config (Soft AP). |
| **Mất điện giữa lúc lưu cấu hình báo thức** | Áp dụng Atomic Write (`.tmp` + `fsync` + `rename`). File cấu hình cũ giữ nguyên vẹn nếu mất điện giữa chừng. |
| **Nhập sai mật khẩu Wi-Fi khi cấu hình Smart Config** | Thử kết nối Station mode trong 20s. Nếu thất bại (không đạt `COMPLETED`), tự động hủy Station và quay lại Soft AP để nhập lại. |

---

## 9. Bảng đối chiếu Requirement Coverage

| Requirement ID | Mô tả ngắn | Đề cập ở mục | Ghi chú |
|---|---|---|---|
| P2-M1 | Driver nút nhấn + cơ chế Smart Config Soft AP | Mục 2, 3, 4, 6 | Driver ngắt GPIO 2 cạnh + debounce 20ms; Userspace phân loại giữ ≥5s chuyển Soft AP; Fallback 20s nếu sai Wi-Fi. |
| P2-M2 | Driver buzzer | Mục 2, 3 | Char driver xuất GPIO output tại `/dev/buzzer_driver`. |
| P2-M3 | Đồng bộ giờ qua NTP | Mục 1, 5 | Dùng `systemd-timesyncd` tự đồng bộ giờ khi kết nối Wi-Fi; `timerfd` + `time()` xử lý an toàn khi nhảy giờ. |
| P2-M4 | Màn hình đồng hồ, giây nhảy đúng | Mục 2, 5 | Dùng `timerfd` bắn mỗi 1s, đọc giờ từ `time()` chống trôi. |
| P2-M5 | Màn hình thời tiết, refresh đúng nhịp | Mục 2, 5, 8 | Fetch dữ liệu khi chuyển màn hình & định kỳ 5 phút; non-blocking socket timeout 5s; hiện icon lỗi khi mất mạng. |
| P2-M6 | Nhấn nút chuyển màn hình | Mục 2, 4 | Nhấn ngắn (50ms - 5000ms) khi còi không kêu sẽ đổi `CLOCK` <-> `WEATHER`. |
| P2-M7 | Webserver cấu hình báo thức | Mục 2, 6 | Webserver HTTP C thuần, ghi đè atomic 1 lịch duy nhất vào `alarm.conf`. |
| P2-M8 | Buzzer kêu đúng giờ | Mục 2, 4, 7 | `clock_thread` so sánh giờ hệ thống và bật `alarm_ringing` kích còi. |
| P2-M9 | Tắt buzzer bằng nút nhấn | Mục 2, 4, 7 | Ưu tiên cao nhất: nhấn ngắn khi còi kêu sẽ tắt còi ngay lập tức. |

---

## 10. Rủi ro em thấy khó nhất trong project này

1. **Chuyển đổi Soft AP ↔ Station mode không bị treo giao diện mạng:** Việc điều khiển luồng khởi động/dừng các system services (`hostapd`, `dnsmasq`, `wpa_supplicant`) qua `fork()` + `execvp()` không chặn đòi hỏi xử lý timeout đếm ngược 20s và fallback quay lại Soft AP chuẩn xác khi nhập sai mật khẩu Wi-Fi.
2. **Phân loại chính xác sự kiện nút nhấn theo 3 ngữ cảnh:** Xử lý sự kiện ngắt GPIO thô từ driver đã qua bộ lọc chống rung 20ms dựa trên mốc thời gian timestamp ($50\text{ ms} \le t < 5000\text{ ms}$ vs. $t \ge 5000\text{ ms}$) và cờ trạng thái `alarm_ringing` để ưu tiên tắt còi báo thức đúng lúc mà không làm lệch luồng chuyển màn hình.
3. **Đảm bảo OLED nhảy giây mượt mà khi các luồng I/O khác đang hoạt động và NTP nhảy giờ:** Giữ cho `clock_thread` (`timerfd`) cập nhật giao diện `HH:MM:SS` đúng nhịp từng giây không bị khựng khi `weather_thread` bị timeout mạng, `webserver_thread` ghi file cấu hình hoặc `systemd-timesyncd` đồng bộ nhảy giờ hệ thống.