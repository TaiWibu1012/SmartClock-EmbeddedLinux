# SMART WEATHER ALARM CLOCK — Embedded Linux

> **Tác giả:** Lê Phúc Tài  
> **Phần cứng mục tiêu:** Raspberry Pi Zero 2W (Broadcom BCM2837 SoC, Quad-Core ARM Cortex-A53 @ 1.0 GHz, 512 MB LPDDR2)  
> **Hệ điều hành:** Custom Embedded Linux (Yocto Project Poky 5.0 LTS Scarthgap, Linux Kernel 6.6.63 LTS, 32-bit ARMv7-A)  

---

## 1. Giới thiệu Tổng quan Dự án

**Smart Weather Alarm Clock** là một thiết bị IoT Edge nhúng hoàn chỉnh được phát triển từ tầng hệ điều hành (Board Support Package), trình điều khiển nhân Linux (Kernel Drivers) cho đến ứng dụng đa luồng trong không gian người dùng (User-space Systems Programming).

Hệ thống được thiết kế và tổ chức thành **hai phiên bản kiến trúc độc lập (V1 và V2)** trong cùng một repository, mang lại cho người dùng và các kỹ sư nhúng hai tùy chọn tiếp cận linh hoạt:

* 📦 **[Tùy chọn 1: Phiên bản V1 (Character Device Driver Architecture)](V1/README.md):**  
  Phiên bản nền tảng cổ điển, tập trung vào kỹ thuật lập trình **Character Device Driver** truyền thống (`/dev/btn_driver`, `/dev/buzzer_driver`), quản lý ngắt hai cạnh với debounce trong kernel, điều khiển PWM bằng `hrtimer`, đồng bộ đa luồng POSIX, web server nhúng (Port 8080) và cơ chế chuyển đổi Soft AP $\leftrightarrow$ Station.  
  👉 **Xem chi tiết tại:** [V1/README.md](V1/README.md)

* 🚀 **[Tùy chọn 2: Phiên bản V2 (Industrial IoT & Linux Subsystem Architecture)](V2/README.md):**  
  Phiên bản nâng cấp đạt chuẩn công nghiệp, chuyển đổi nút nhấn sang **Linux Input Subsystem (`evdev`)**, chẩn đoán qua **Sysfs telemetry**, bảo vệ hệ thống bằng **Hardware Watchdog (`/dev/watchdog`)**, tối ưu hóa băng thông I2C bằng kỹ thuật **Dirty-Page** trên màn hình OLED SSD1306, vẽ đồ họa 2D Bresenham số nguyên, lấy thời tiết thực tế qua **Open-Meteo HTTPS (OpenSSL)** và kết nối đám mây hai chiều qua **MQTT-C (HiveMQ)**.  
  👉 **Xem chi tiết tại:** [V2/README.md](V2/README.md)

* 💾 **[Hệ điều hành Yocto Linux (Flashable OS Images)](OS/README.md):**  
  Bản build OS nhúng tối ưu dung lượng được đóng gói sẵn dưới dạng ảnh thẻ nhớ bootable (`.wic.bz2` ~55 MB kèm `.bmap`), tích hợp sẵn Dropbear SSH, Wi-Fi driver Broadcom BCM43436, UART Serial Console 115200 và các công cụ chẩn đoán hệ thống.  
  👉 **Xem chi tiết tại:** [OS/README.md](OS/README.md)

---

## 2. Bảng So sánh Kỹ thuật giữa V1 và V2

| Tiêu chí Kỹ thuật | Phiên bản V1 (Foundation) | Phiên bản V2 (Industrial IoT) |
|---|---|---|
| **Driver Nút nhấn (GPIO 17)** | Character Device Driver riêng (`/dev/btn_driver`) | Chuẩn **Linux Input Subsystem** (`/dev/input/event*`, `KEY_POWER`) |
| **Giám sát & Chẩn đoán Driver** | Log kernel qua `dmesg` | **Sysfs Telemetry** (`/sys/devices/platform/.../press_count`, `debounce_drops`) |
| **Bảo vệ Hệ thống (Fail-Safe)** | Xử lý lỗi trong user-space | **Hardware Watchdog phần cứng** (`/dev/watchdog`, 15s timeout, 'V' magic close) |
| **Cơ chế Hiển thị OLED (I2C)** | Gửi toàn bộ Framebuffer mỗi chu kỳ | **Dirty-Page Buffer** (So sánh shadow buffer, giảm 75% – 90% lưu lượng I2C) |
| **Thư viện Đồ họa 2D** | Bitmap cố định | Thuật toán **Bresenham số nguyên** (đường thẳng, đường tròn) + Bitmap icon 1-bit |
| **Nguồn Dữ liệu Thời tiết** | Mock Server nội bộ (Port 8081) | **Open-Meteo REST API qua OpenSSL HTTPS** (tự chuyển Mock khi mất mạng) |
| **Kết nối Cloud IoT** | Không có | **MQTT-C Client** gửi Telemetry & nhận lệnh điều khiển qua broker HiveMQ |
| **Driver Còi Buzzer (GPIO 27)** | `hrtimer` PWM 2000 Hz (`/dev/buzzer_driver`) | `hrtimer` PWM 2000 Hz (`/dev/buzzer_driver`) |
| **Cấu hình Mạng Wi-Fi** | SmartConfig (Soft AP `192.168.4.1` $\leftrightarrow$ Station) | SmartConfig (Soft AP `192.168.4.1` $\leftrightarrow$ Station) |
| **Giao diện Web Cấu hình** | Web Server C Socket (Port 8080) | Web Server C Socket (Port 8080) |
| **Lưu trữ Cấu hình Báo thức** | Kỹ thuật Atomic Write (`.tmp` $\rightarrow$ `fsync` $\rightarrow$ `rename`) | Kỹ thuật Atomic Write (`.tmp` $\rightarrow$ `fsync` $\rightarrow$ `rename`) |

---

## 3. Sơ đồ Nối chân Phần cứng (Hardware Pinout)

Sơ đồ phần cứng dùng chung cho cả hai phiên bản V1 và V2:

| Thiết bị Ngoại vi | Chân SoC (BCM) | Chân Header (Physical Pin) | Chế độ Hoạt động / Giao thức | Chức năng |
|---|---|---|---|---|
| **Nút nhấn (Button)** | `GPIO 17` | Pin 11 | Input, Active-Low, Internal Pull-Up, Dual-Edge IRQ | Nút bấm đa chức năng theo ngữ cảnh |
| **Còi Buzzer** | `GPIO 27` | Pin 13 | Output, Active-High, PWM/hrtimer 2000 Hz | Phát âm thanh chuông báo thức |
| **OLED SSD1306 SDA** | `GPIO 2` | Pin 3 | I2C-1 Data (`/dev/i2c-1`, Address `0x3C`) | Kênh truyền dữ liệu màn hình hiển thị |
| **OLED SSD1306 SCL** | `GPIO 3` | Pin 5 | I2C-1 Clock (`/dev/i2c-1`, Address `0x3C`) | Kênh xung nhịp I2C màn hình hiển thị |
| **UART Serial Debug TX** | `GPIO 14` | Pin 8 | Serial TX (115200 baud, 8N1) | Giao tiếp dòng lệnh qua cáp USB-to-TTL |
| **UART Serial Debug RX** | `GPIO 15` | Pin 10 | Serial RX (115200 baud, 8N1) | Giao tiếp dòng lệnh qua cáp USB-to-TTL |
| **Nguồn VCC (3.3V)** | `3.3V` | Pin 1 | Power Rail 3.3V | Cấp nguồn OLED, Nút nhấn, Buzzer |
| **Nối đất (GND)** | `GND` | Pin 6, 9 | Ground Rail | Tiếp địa chung toàn bộ mạch |

---

## 4. Kiến trúc Hệ điều hành Yocto Linux (Custom Embedded OS)

Cả hai phiên bản V1 và V2 đều chạy trực tiếp trên bản phân phối Linux nhúng được build riêng bằng **Yocto Project (Poky 5.0 LTS Scarthgap)**:

* **Kiến trúc CPU:** **ARM 32-bit (ARMv7-A / armv7l)**  
  *(Target Machine: `MACHINE = "raspberrypi0-2w"`, tối ưu theo `cortexa7thf-neon-vfpv4`, nhân Linux dạng `zImage` 32-bit).*
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

## 5. Cấu trúc Thư mục Dự án

```text
SmartClock/
├── README.md                          # Tài liệu tổng quan dự án, so sánh V1 vs V2, sơ đồ chân & OS
├── .gitignore                         # Bộ lọc git (bỏ qua file nhị phân, object, ảnh thẻ nhớ lớn)
│
├── OS/                                # Thư mục chứa file ảnh hệ điều hành Yocto nhúng
│   ├── README.md                      # Chi tiết cấu hình bản build Yocto và hướng dẫn nạp thẻ nhớ
│   ├── core-image-minimal-*.wic.bz2   # File ảnh đĩa bootable hoàn chỉnh (55 MB)
│   ├── core-image-minimal-*.wic.bmap  # Block map file hỗ trợ bmaptool ghi đĩa siêu tốc
│   └── core-image-minimal-*.tar.bz2   # File nén toàn bộ Rootfs hệ thống (34 MB)
│
├── V1/                                # TÙY CHỌN 1: Character Device Driver Architecture
│   ├── README.md                      # Hướng dẫn chi tiết biên dịch, nạp driver và vận hành V1
│   ├── devicetree/                    # Source DTS và binary DTBO overlay cho V1
│   ├── docs/                          # Báo cáo kiểm thử, log Valgrind, Helgrind và video demo V1
│   ├── include/                       # Header chung giữa Kernel driver và Userspace
│   ├── src/
│   │   ├── Makefile                   # Top-level Makefile biên dịch V1
│   │   ├── driver/                    # Driver nút nhấn (/dev/btn_driver) & còi buzzer
│   │   └── app/                       # Ứng dụng đa luồng V1 (timerfd, webserver, SNTP)
│   └── systemd/                       # File service tự khởi động cùng OS
│
└── V2/                                # TÙY CHỌN 2: Industrial IoT & Linux Subsystem Architecture
    ├── README.md                      # Hướng dẫn chi tiết biên dịch, nạp driver và vận hành V2
    ├── devicetree/                    # Source DTS và binary DTBO overlay cho V2
    ├── include/                       # Header chung giữa Kernel driver và Userspace
    ├── src/
    │   ├── Makefile                   # Top-level Makefile biên dịch V2
    │   ├── driver/                    # Driver nút nhấn chuẩn Input Subsystem (evdev) & Buzzer
    │   └── app/                       # Ứng dụng đa luồng V2 (Watchdog, Dirty-Page, Open-Meteo, MQTT-C)
    └── systemd/                       # File service tự khởi động cùng OS
```

---

## 6. Hướng dẫn Bắt đầu Nhanh (Quick Start)

### Bước 1: Ghi Hệ điều hành vào Thẻ nhớ MicroSD
Sử dụng công cụ `bmaptool` để ghi file ảnh đĩa từ thư mục `OS/` vào thẻ nhớ:
```bash
sudo apt-get install -y bmap-tools
cd OS/
sudo bmaptool copy core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2 /dev/sdX   # Thay /dev/sdX bằng thẻ nhớ của bạn
```
Chi tiết các cách ghi khác (lệnh `dd`, phần mềm Raspberry Pi Imager), vui lòng tham khảo [OS/README.md](OS/README.md).

### Bước 2: Khởi động Raspberry Pi Zero 2W
1. Cắm thẻ nhớ vào Pi Zero 2W.
2. Kết nối mạch USB-to-TTL vào chân GPIO 14 (Pin 8 TX), GPIO 15 (Pin 10 RX) và GND (Pin 6/9).
3. Mở Serial Console tại baud rate **115200** để đăng nhập với user `root`.
4. Bảo đảm file `/boot/config.txt` đã kích hoạt bus I2C: `dtparam=i2c_arm=on`.

### Bước 3: Lựa chọn và Chạy Phiên bản mong muốn
* Nếu bạn muốn trải nghiệm kiến trúc **Character Device Drivers** truyền thống:
  👉 Vào thư mục `V1/` và làm theo hướng dẫn tại **[V1/README.md](V1/README.md)**.
* Nếu bạn muốn trải nghiệm kiến trúc **Linux Input Subsystem, Watchdog, Dirty-Page & Cloud MQTT**:
  👉 Vào thư mục `V2/` và làm theo hướng dẫn tại **[V2/README.md](V2/README.md)**.
