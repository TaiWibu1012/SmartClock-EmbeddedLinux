# SMART WEATHER ALARM CLOCK — v1.0

> **Tác giả:** Lê Phúc Tài  
> **Phiên bản:** v1.0 (Character Device Driver Architecture)  
> **Phần cứng mục tiêu:** Raspberry Pi Zero 2W (Broadcom BCM2837 SoC, Quad-Core ARM Cortex-A53 @ 1.0 GHz, 512 MB LPDDR2)  
> **Hệ điều hành:** Custom Embedded Linux (Yocto Project Poky 5.0 LTS Scarthgap, Linux Kernel 6.6.63 LTS, 32-bit ARMv7-A)  

---

## 1. Giới thiệu Kiến trúc Phiên bản v1.0

**Smart Weather Alarm Clock v1.0** là phiên bản nền tảng của thiết bị đồng hồ thông minh kiêm trạm thông tin thời tiết nhúng trên **Raspberry Pi Zero 2W**. Dự án được xây dựng dựa trên kiến trúc **Character Device Drivers** truyền thống kết hợp với ứng dụng đa luồng trong không gian người dùng:

1. **Lập trình Driver Nhân Linux (Kernel Space):**
   * **Driver Nút nhấn Đa chức năng (`/dev/btn_driver`):** Đăng ký character device driver xử lý ngắt 2 cạnh (`IRQ_TYPE_EDGE_BOTH`) từ nút bấm vật lý (GPIO 17). Tích hợp thuật toán lọc rung phím (debounce 20ms) trong kernel và ghi nhận timestamp sự kiện với độ chính xác cao bằng `ktime_get_ns()`.
   * **Driver Còi Buzzer (`/dev/buzzer_driver`):** Sử dụng `hrtimer` (high-resolution timer) trong nhân Linux để xuất chuỗi xung vuông tần số chính xác 2000 Hz điều khiển còi buzzer phát âm thanh cảnh báo.
2. **Lập trình Hệ thống Đa luồng Không gian Người dùng (User Space):**
   * **Đồng hồ chạy thời gian thực (`timerfd`):** Định thời chính xác 1 giây mỗi nhịp, không trôi giờ (drift-free) thông qua file descriptor của kernel.
   * **Đồng bộ Thời gian chuẩn NTP:** Tự động kết nối tới NTP server qua giao thức UDP khi có mạng internet để đồng bộ giờ theo múi giờ Việt Nam (UTC+7).
   * **Quản lý Mạng Thông minh (SmartConfig):** Tự động chuyển đổi giữa chế độ trạm phát **Soft AP** (`wpa_supplicant` mode=2 + `udhcpd`) để cấp IP và cấu hình, và chế độ thu sóng **Station** (`wpa_supplicant` + `udhcpc`). Có cơ chế timeout 20s tự phục hồi về Soft AP nếu nhập sai mật khẩu Wi-Fi.
   * **Máy chủ Web Cấu hình Nhúng (Port 8080):** Xây dựng bằng socket C thuần, cung cấp giao diện Web trực quan trên trình duyệt điện thoại/máy tính để quét Wi-Fi và cài đặt giờ báo thức.
   * **Lưu cấu hình Bền vững bằng Atomic Write:** Cơ chế ghi an toàn (`.tmp` $\rightarrow$ `fsync` $\rightarrow$ `rename`) vào `/etc/smartclock/alarm.conf`, đảm bảo không bị hỏng file cấu hình nếu mất điện đột ngột.
   * **Màn hình OLED SSD1306 (128x64):** Giao tiếp trực tiếp qua bus I2C-1 (`/dev/i2c-1`, địa chỉ `0x3C`), hỗ trợ 2 giao diện hiển thị: Màn hình Đồng hồ và Màn hình Thời tiết.

---

## 2. Kiến trúc Hệ điều hành Yocto Linux (Custom Embedded OS)

Hệ điều hành nhúng chạy trên Raspberry Pi Zero 2W được xây dựng tùy biến hoàn toàn bằng **Yocto Project (Poky 5.0 LTS Scarthgap)**:

* **Kiến trúc CPU:** **ARM 32-bit (ARMv7-A / armv7l)**  
  *(Cấu hình `MACHINE = "raspberrypi0-2w"`, tối ưu theo `cortexa7thf-neon-vfpv4`, nhân Linux được biên dịch dạng `zImage` 32-bit).*
* **Bản phân phối (Distribution):** Poky Scarthgap 5.0 LTS.
* **Linux Kernel:** Phiên bản 6.6.63 LTS.
* **Các Meta Layers được sử dụng (`bblayers.conf`):**
  1. `meta`: Core metadata từ OpenEmbedded, cung cấp cross-toolchain (GCC, Binutils, Glibc), busybox và cấu trúc hệ thống tệp cốt lõi.
  2. `meta-poky`: Cấu hình chính sách và bản phân phối Poky tham chiếu của Yocto Project.
  3. `meta-yocto-bsp`: Các Board Support Package tham chiếu tiêu chuẩn của Yocto Project.
  4. `meta-raspberrypi`: BSP chính thức cho phần cứng Raspberry Pi (chứa firmware GPU Broadcom VideoCore, kernel Pi Zero 2W, Device Tree và driver Wi-Fi/Bluetooth).
  5. `meta-openembedded/meta-oe`: Layer mở rộng cung cấp các công cụ mạng, thư viện và tiện ích dòng lệnh (`htop`, `net-tools`,...).
* **Các gói phần mềm tích hợp sẵn (`IMAGE_INSTALL`):**
  * `dropbear`: Máy chủ SSH siêu nhẹ (hoạt động mặc định tại cổng 22).
  * `wpa_supplicant`, `iw`, `wireless-regdb-static`: Bộ công cụ quản lý và kết nối mạng Wi-Fi.
  * `linux-firmware-rpidistro-bcm43436`: Firmware điều khiển chip Wi-Fi/Bluetooth Broadcom trên Pi Zero 2W.
  * `net-tools`, `htop`: Công cụ theo dõi tiến trình và cấu hình mạng.
  * `kernel-modules`, `kernel-devsrc`: Header và module nhân phục vụ nạp driver out-of-tree.
* **Giao tiếp Serial Debug (UART Console):** Kích hoạt sẵn (`ENABLE_UART = "1"`). Baud rate: **115200**, 8N1 trên chân GPIO 14 (TX) và GPIO 15 (RX).
* **Tài khoản đăng nhập mặc định:**
  * **User:** `root`
  * **Mật khẩu:** *Không có mật khẩu (trống)*

---

## 3. Sơ đồ Nối chân Phần cứng (Hardware Pinout)

| Thiết bị Ngoại vi | Chân SoC (BCM) | Chân Header (Physical Pin) | Chế độ Hoạt động / Giao thức | Chức năng |
|---|---|---|---|---|
| **Nút nhấn (Button)** | `GPIO 17` | Pin 11 | Input, Active-Low, Internal Pull-Up, Dual-Edge IRQ | Character device `/dev/btn_driver` |
| **Còi Buzzer** | `GPIO 27` | Pin 13 | Output, Active-High, PWM/hrtimer 2000 Hz | Character device `/dev/buzzer_driver` |
| **OLED SSD1306 SDA** | `GPIO 2` | Pin 3 | I2C-1 Data (`/dev/i2c-1`, Address `0x3C`) | Kênh truyền dữ liệu màn hình hiển thị |
| **OLED SSD1306 SCL** | `GPIO 3` | Pin 5 | I2C-1 Clock (`/dev/i2c-1`, Address `0x3C`) | Kênh xung nhịp I2C màn hình hiển thị |
| **UART Serial Debug TX** | `GPIO 14` | Pin 8 | Serial TX (115200 baud, 8N1) | Giao tiếp dòng lệnh qua cáp USB-to-TTL |
| **UART Serial Debug RX** | `GPIO 15` | Pin 10 | Serial RX (115200 baud, 8N1) | Giao tiếp dòng lệnh qua cáp USB-to-TTL |
| **Nguồn VCC (3.3V)** | `3.3V` | Pin 1 | Power Rail 3.3V | Cấp nguồn OLED, Nút nhấn, Buzzer |
| **Nối đất (GND)** | `GND` | Pin 6, 9 | Ground Rail | Tiếp địa chung toàn bộ mạch |

---

## 4. Cấu trúc Thư mục Phiên bản v1.0

```text
SmartClock/
├── README.md                          # Tài liệu kỹ thuật, sơ đồ chân và hướng dẫn vận hành v1.0
├── OS/                                # Thư mục chứa file ảnh hệ điều hành Yocto nhúng
│   ├── README.md                      # Chi tiết cấu hình bản build Yocto và hướng dẫn nạp thẻ nhớ
│   ├── core-image-minimal-*.wic.bz2   # File ảnh đĩa bootable hoàn chỉnh (55 MB)
│   ├── core-image-minimal-*.wic.bmap  # Block map file hỗ trợ bmaptool ghi đĩa siêu tốc
│   └── core-image-minimal-*.tar.bz2   # File nén toàn bộ Rootfs hệ thống (34 MB)
├── devicetree/
│   ├── smartclock-overlay.dts         # Source Device Tree Overlay (GPIO 17, 27, I2C-1)
│   └── smartclock-overlay.dtbo        # Binary Device Tree Blob đã biên dịch
├── docs/
│   ├── test_report.md                 # Báo cáo kết quả kiểm thử chức năng & phân tích hiệu năng
│   ├── debug_logs/                    # Log kiểm thử GDB, Helgrind, Valgrind, Strace
│   └── demo/                          # Đường dẫn video demo thực tế
├── include/
│   └── smartclock_common.h            # Header định nghĩa dùng chung Kernel & Userspace
├── src/
│   ├── Makefile                       # Top-level Makefile biên dịch toàn bộ dự án
│   ├── driver/
│   │   ├── Makefile                   # Kbuild Makefile cho Kernel Modules
│   │   ├── btn_driver.c               # Driver nút nhấn Character Device (/dev/btn_driver)
│   │   └── buzzer_driver.c            # Driver còi buzzer Character Device (/dev/buzzer_driver)
│   └── app/
│       ├── Makefile                   # Makefile biên dịch ứng dụng Userspace
│       ├── main.c                     # Khởi tạo hệ thống, điều phối luồng
│       ├── system_state.h             # Cấu trúc Shared State & Mutex boundary
│       ├── clock_screen.c/.h          # Màn hình đồng hồ (chạy timerfd 1 giây đều đặn)
│       ├── weather_screen.c/.h        # Màn hình thời tiết (socket non-blocking timeout 5s)
│       ├── alarm_manager.c/.h         # Quản lý báo thức, kích còi, Atomic Write cấu hình
│       ├── smartconfig.c/.h           # Quản lý mạng Soft AP <-> Station, SNTP client
│       ├── webserver.c/.h             # HTTP server cấu hình nhúng (Port 8080)
│       └── ssd1306_oled.c/.h          # Driver điều khiển OLED qua I2C-1
└── systemd/
    └── smartclock.service             # Systemd Unit File tự khởi động cùng hệ điều hành
```

---

## 5. Ma trận Xử lý Nút nhấn theo Ngữ cảnh (Button Priority Matrix)

Nút nhấn vật lý (GPIO 17) được quản lý tập trung và phân loại sự kiện nguyên tử dưới một Lock Mutex duy nhất (`g_state_mutex`):

```mermaid
flowchart TD
    A[Sự kiện Nút nhấn] --> B{Thời gian bấm}
    B -- "t >= 5000 ms (Nhấn giữ)" --> C[Ưu tiên 0: Chuyển chế độ Smart Config Soft AP <-> Station]
    B -- "50 ms <= t < 5000 ms (Nhấn ngắn)" --> D{Còi báo thức đang kêu?}
    D -- "Có (alarm_ringing == true)" --> E[Ưu tiên 1: Tắt chuông báo thức ngay lập tức]
    D -- "Không (alarm_ringing == false)" --> F[Ưu tiên 2: Chuyển đổi màn hình CLOCK <-> WEATHER]
```

| Mức Ưu tiên | Ngữ cảnh Hệ thống | Hành động Nhấn | Kết quả Xử lý |
|---|---|---|---|
| **Ưu tiên 0** *(Ngắt ngầm)* | Bất kỳ lúc nào (Chuông kêu hoặc không) | Nhấn giữ $\ge 5\text{s}$ | Chuyển đổi qua lại giữa **Soft AP** và **Station**. Nếu còi đang kêu, tự động tắt còi trước khi chuyển mạng. |
| **Ưu tiên 1** *(Cao nhất khi reo)* | Còi báo thức đang kêu (`alarm_ringing == true`) | Nhấn ngắn ($< 5\text{s}$) | **Tắt còi báo thức ngay lập tức** (`alarm_ringing = false`). Giữ nguyên màn hình hiện tại. Lần nhấn tiếp theo quay về luồng thường. |
| **Ưu tiên 2** *(Bình thường)* | Còi không kêu (`alarm_ringing == false`) | Nhấn ngắn ($< 5\text{s}$) | **Chuyển đổi màn hình** hiển thị trên OLED (`CLOCK` $\leftrightarrow$ `WEATHER`). |

---

## 6. Hướng dẫn Triển khai & Vận hành v1.0

### 6.1 Ghi Hệ điều hành Yocto vào Thẻ nhớ MicroSD

File ảnh đĩa Yocto Linux hoàn chỉnh được lưu sẵn trong thư mục `OS/` (hoặc tải về từ mục **Releases** trên GitHub).

> **Lưu ý an toàn:** Thay `/dev/sdX` bằng đường dẫn chính xác của thẻ nhớ microSD trên máy tính của bạn (kiểm tra bằng lệnh `lsblk`).

#### Cách A: Dùng `bmaptool` (Khuyên dùng — Tốc độ nhanh nhất)
`bmaptool` đọc block map `.bmap` để bỏ qua các vùng nhớ trống, hoàn tất ghi đĩa chỉ trong 20–30 giây:
```bash
sudo apt-get install -y bmap-tools
cd OS/
sudo bmaptool copy core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2 /dev/sdX
```

#### Cách B: Dùng lệnh `dd` truyền thống
```bash
cd OS/
bunzip2 -k core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2
sudo dd if=core-image-minimal-raspberrypi0-2w.rootfs.wic of=/dev/sdX bs=4M status=progress conv=fsync
```

#### Cách C: Dùng phần mềm Raspberry Pi Imager
1. Giải nén file `.wic.bz2` thành `.wic`, đổi đuôi thành `.img`.
2. Mở **Raspberry Pi Imager** $\rightarrow$ chọn **Use Custom** $\rightarrow$ chọn file `.img` $\rightarrow$ chọn thẻ microSD $\rightarrow$ bấm **Write**.

---

### 6.2 Khởi động lần đầu & Thiết lập Kết nối trên Pi Zero 2W

1. **Kết nối Serial Console (UART):**
   * Sử dụng mạch chuyển đổi USB-to-TTL nối vào Raspberry Pi Zero 2W:
     * Cáp TX $\rightarrow$ Chân 10 (GPIO 15 - RX của Pi)
     * Cáp RX $\rightarrow$ Chân 8 (GPIO 14 - TX của Pi)
     * Cáp GND $\rightarrow$ Chân 6 hoặc 9 (GND của Pi)
   * Mở terminal serial với cấu hình: Baud rate **115200**, Data bits 8, Stop bits 1, Parity None.
   * Cắm nguồn vào cổng micro-USB (PWR IN). Đăng nhập bằng tài khoản: `root` (không cần mật khẩu).

2. **Kích hoạt giao tiếp I2C-1:**
   * Kiểm tra file cấu hình boot `/boot/config.txt`:
     ```bash
     cat /boot/config.txt | grep i2c_arm
     ```
   * Nếu chưa có, thêm dòng sau vào `/boot/config.txt`:
     ```text
     dtparam=i2c_arm=on
     ```
   * Sau khi khởi động lại, kiểm tra node thiết bị: `ls -l /dev/i2c-1` để chắc chắn bus I2C-1 đã sẵn sàng.

3. **Kết nối mạng Wi-Fi & SSH:**
   * Máy chủ SSH **Dropbear** đã được tích hợp sẵn trong OS và tự khởi chạy trên cổng 22.
   * Để kết nối Wi-Fi thủ công bằng `wpa_supplicant`:
     ```bash
     wpa_passphrase "Ten_WiFi" "Mat_Khau" >> /etc/wpa_supplicant.conf
     wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
     udhcpc -i wlan0
     ```
   * Ghi nhận địa chỉ IP nhận được qua lệnh `ifconfig wlan0`. Giờ đây bạn có thể đăng nhập qua SSH: `ssh root@<IP_CUA_PI>`.

---

### 6.3 Cài đặt Device Tree Overlay

Device Tree Overlay cấu hình các chân GPIO 17 (Nút nhấn), GPIO 27 (Buzzer) và kích hoạt bus I2C-1:

```bash
# 1. Sao chép file dtbo vào thư mục overlays của bootloader
cp devicetree/smartclock-overlay.dtbo /boot/overlays/

# 2. Thêm chỉ thị nạp overlay vào /boot/config.txt để nạp tự động mỗi khi khởi động
echo "dtoverlay=smartclock-overlay" >> /boot/config.txt

# (Tùy chọn) Nạp overlay động ngay lập tức mà không cần reboot:
dtoverlay devicetree/smartclock-overlay.dtbo
```

---

### 6.4 Biên dịch & Triển khai Ứng dụng (Deploy)

* **Biên dịch trên máy tính phát triển:**
  ```bash
  cd src/
  make clean
  make all      # Biên dịch driver Character Device và ứng dụng Userspace v1.0
  ```

* **Triển khai sang Pi Zero 2W qua SCP:**
  ```bash
  scp -r ../SmartClock root@<IP_CUA_PI>:/home/root/
  ```

---

### 6.5 Nạp Driver & Khởi chạy Ứng dụng v1.0

```bash
# 1. Nạp 2 driver character device
insmod src/driver/btn_driver.ko       # Tạo node /dev/btn_driver
insmod src/driver/buzzer_driver.ko    # Tạo node /dev/buzzer_driver

# 2. Phân quyền truy cập các file thiết bị
chmod 666 /dev/btn_driver /dev/buzzer_driver /dev/i2c-1

# 3. Tạo thư mục cấu hình hệ thống
mkdir -p /etc/smartclock /etc/wpa_supplicant /var/lib/misc

# 4. Khởi chạy ứng dụng v1.0
./src/app/smartclock
```

#### Cài đặt Tự khởi động cùng Hệ điều hành (Systemd Service)
```bash
cp systemd/smartclock.service /etc/systemd/system/
cp src/app/smartclock /usr/bin/
systemctl daemon-reload
systemctl enable smartclock.service
systemctl start smartclock.service
```

---

### 6.6 Hướng dẫn Sử dụng Thực tế

#### 1. Thao tác Phím Bấm Vật lý (GPIO 17)
* **Chuyển đổi màn hình (Nhấn ngắn $< 5\text{s}$):** Luân phiên chuyển đổi giữa màn hình **ĐỒNG HỒ** (giờ, phút, giây, icon chuông) và màn hình **THỜI TIẾT** (nhiệt độ, độ ẩm).
* **Tắt còi báo thức (Nhấn ngắn khi chuông đang reo):** Tắt còi buzzer ngay lập tức mà không làm nhảy sai màn hình hiển thị.
* **Cấu hình Wi-Fi SmartConfig (Nhấn giữ $\ge 5\text{s}$):** Chuyển Pi sang chế độ trạm phát sóng **Soft AP** (Tên mạng: `SmartClock-AP`, IP: `192.168.4.1`). Nhấn giữ 5s lần nữa để quay lại chế độ bắt sóng Station.

#### 2. Cấu hình qua Trình duyệt Web (Port 8080)
1. Kết nối điện thoại hoặc máy tính vào mạng Wi-Fi của SmartClock (hoặc cùng mạng LAN với Pi).
2. Mở trình duyệt web truy cập: `http://<IP_CUA_PI>:8080` (hoặc `http://192.168.4.1:8080` khi ở chế độ Soft AP).
3. Giao diện Web cho phép:
   * Nhập thông tin SSID / Mật khẩu Wi-Fi để đồng hồ kết nối internet.
   * Đặt giờ báo thức (Giờ, Phút) và Bật/Tắt chuông. Cấu hình được ghi bền vững vào `/etc/smartclock/alarm.conf` bằng kỹ thuật Atomic Write chống mất dữ liệu khi mất nguồn đột ngột.
