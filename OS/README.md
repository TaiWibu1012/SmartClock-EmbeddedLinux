# Yocto Linux OS Image cho Raspberry Pi Zero 2W

Thư mục này chứa file ảnh hệ điều hành nhúng (Embedded Linux) được build riêng bằng **Yocto Project (Poky 5.0 LTS "Scarthgap")** cho phần cứng **Raspberry Pi Zero 2W**.

---

## 1. Thông tin Cấu hình Bản build OS

* **Kiến trúc CPU:** **ARM 32-bit (ARMv7-A / armv7l)**  
  *(Cấu hình `MACHINE = "raspberrypi0-2w"`, tối ưu theo kiến trúc `cortexa7thf-neon-vfpv4`, sinh kernel dạng `zImage` 32-bit).*
* **Bản phân phối (Distro):** Poky Scarthgap 5.0 LTS
* **Linux Kernel:** 6.6.63 LTS
* **Cơ chế Boot:** MBR 2 phân vùng (Phân vùng 1: FAT32 `/boot` chứa bootloader VC4 GPU & kernel; Phân vùng 2: ext4 `/` root filesystem).
* **Các Meta Layers được sử dụng (`bblayers.conf`):**
  1. `meta`: Core metadata từ OpenEmbedded, cung cấp toolchain cross-compile, glibc, busybox và các gói hệ thống cốt lõi.
  2. `meta-poky`: Cấu hình chính sách và bản phân phối Poky tham chiếu của Yocto Project.
  3. `meta-yocto-bsp`: Các Board Support Package phần cứng tham chiếu chuẩn.
  4. `meta-raspberrypi`: BSP chính thức cho phần cứng Raspberry Pi (chứa recipes cho firmware Broadcom VideoCore, Linux kernel Raspberry Pi, Device Tree, và firmware Wi-Fi/Bluetooth).
  5. `meta-openembedded/meta-oe`: Layer mở rộng bổ sung các công cụ mạng, thư viện phát triển và tiện ích dòng lệnh.
* **Các gói phần mềm tích hợp sẵn (`IMAGE_INSTALL`):**
  * `dropbear`: Lightweight SSH server (chạy mặc định cổng 22).
  * `wpa_supplicant`, `iw`, `wireless-regdb-static`: Bộ công cụ quản lý và kết nối Wi-Fi.
  * `linux-firmware-rpidistro-bcm43436`: Firmware cho chip Wi-Fi/Bluetooth Broadcom BCM43436 trên Pi Zero 2W.
  * `net-tools`, `htop`: Tiện ích theo dõi tiến trình và cấu hình mạng (ifconfig, route, netstat).
  * `kernel-modules`, `kernel-devsrc`: Header và module kernel phục vụ nạp driver out-of-tree.
* **Cổng giao tiếp Serial (UART Console):** Kích hoạt sẵn (`ENABLE_UART = "1"`). Baud rate: **115200**, 8N1 trên chân GPIO 14 (TX) và GPIO 15 (RX).
* **Tài khoản đăng nhập mặc định:**
  * **User:** `root`
  * **Mật khẩu:** *Không có mật khẩu (trống)*

---

## 2. Danh sách File trong Thư mục

| Tên File | Dung lượng | Mô tả |
|---|---|---|
| `core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2` | ~55 MB | **Ảnh đĩa bootable hoàn chỉnh** nén bằng bzip2. Chứa cả phân vùng Boot FAT32 và Rootfs ext4, dùng để nạp trực tiếp vào thẻ microSD. |
| `core-image-minimal-raspberrypi0-2w.rootfs.wic.bmap` | ~3.3 KB | Block map file, sử dụng cùng công cụ `bmaptool` để ghi ảnh thẻ nhớ siêu tốc và kiểm tra tính toàn vẹn từng block. |
| `core-image-minimal-raspberrypi0-2w.rootfs-20260805091053.tar.bz2` | ~34 MB | File nén toàn bộ Root Filesystem (Rootfs), dùng để trích xuất file, chroot hoặc kiểm tra các package hệ thống. |

---

## 3. Hướng dẫn Ghi OS vào Thẻ nhớ MicroSD

> **Lưu ý:** Thẻ nhớ nên dùng loại microSD Class 10 hoặc UHS-I từ 4 GB trở lên. Thay thế `/dev/sdX` bằng tên thiết bị thẻ nhớ thực tế trên máy tính Linux của bạn (ví dụ `/dev/sdb`, `/dev/mmcblk0`). **Không ghi nhầm vào ổ cứng máy tính.**

### Cách 1: Dùng `bmaptool` (Khuyên dùng — Nhanh & Tự động giải nén)
`bmaptool` đọc file `.bmap` để chỉ ghi các block có dữ liệu, bỏ qua các block trống, giúp thời gian nạp chỉ mất khoảng 20–30 giây:

```bash
# Cài đặt bmaptool nếu chưa có (Ubuntu/Debian)
sudo apt install bmap-tools

# Ghi trực tiếp file .wic.bz2 vào thẻ nhớ
sudo bmaptool copy core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2 /dev/sdX
```

### Cách 2: Giải nén và ghi bằng lệnh `dd`
```bash
# 1. Giải nén file ảnh ra file .wic (khoảng 224 MB)
bunzip2 -k core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2

# 2. Ghi ra thẻ nhớ bằng lệnh dd
sudo dd if=core-image-minimal-raspberrypi0-2w.rootfs.wic of=/dev/sdX bs=4M status=progress conv=fsync
```

### Cách 3: Dùng Raspberry Pi Imager hoặc Balena Etcher
1. Giải nén file `core-image-minimal-raspberrypi0-2w.rootfs.wic.bz2` để thu được file `.wic`.
2. Đổi tên phần mở rộng từ `.wic` thành `.img` (ví dụ `smartclock-os.img`).
3. Mở **Raspberry Pi Imager** $\rightarrow$ chọn **Use custom image** $\rightarrow$ trỏ đến file `.img` $\rightarrow$ chọn thẻ nhớ $\rightarrow$ bấm **Write**.
