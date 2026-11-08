# Maryati Bridge

Maryati Bridge adalah alat bantu diagnostik jaringan dan jembatan Layer-2 (Transparent USB-to-Wi-Fi LAN Bridge) berbasis mikrokontroler ESP32-S3 atau ESP32-S2. Alat ini dirancang untuk menjembatani perangkat Wi-Fi (seperti laptop teknisi, PC kedua, atau alat uji) agar bisa masuk secara transparan ke dalam jaringan LAN kabel milik sebuah PC Host lewat antarmuka USB.

Fungsinya persis seperti kamu mencolokkan kabel LAN langsung ke switch atau router fisik, tetapi semuanya dilakukan secara nirkabel dari sisi klien Wi-Fi.

## Spesifikasi Performa dan Kompatibilitas

* Kapasitas Klien Wi-Fi: Untuk menjaga kualitas, maksimal 4 klien terhubung secara bersamaan.
* Target Throughput: Teruji stabil di atas 10 Mbps (TCP/UDP berkelanjutan).
* Latensi Jembatan: Penambahan latensi kurang dari 5 ms.
* Kompatibilitas OS PC Host: Windows 10/11 (x64) dan Linux (Kernel 5.4+ dengan iproute2/NetworkManager) menggunakan driver jaringan CDC-NCM bawaan OS.

## Fitur Utama (Tri-Mode Adaptive)

Karena sistem operasi modern biasanya memblokir jaringan USB baru dengan alasan keamanan, Maryati Bridge punya 3 mode pintar yang bisa disesuaikan dengan kebutuhanmu:

### Mode 0: P2P Fallback & Local Proxy (Mode Bawaan)

Mode ini otomatis menyala saat alat dicolokkan pertama kali. Sangat berguna kalau kamu tidak punya akses Administrator di PC Host.
* Membuat jaringan lokal sendiri (subnet 192.168.33.x) antara perangkat Wi-Fi dan PC Host.
* DHCP Tanpa Gateway: Server DHCP perangkat ini dilarang memberikan Default Gateway ke PC Host. Ini menjamin koneksi internet atau jaringan asli di PC Host tidak akan terputus atau bentrok.
* Portable Proxy: Terdapat aplikasi proxy (statis dari Golang) di dalam USB Flash Drive-nya. Kamu bisa jalankan script Start_Proxy tanpa hak admin untuk mengakses HTTP/HTTPS dan SOCKS5 ke LAN atau internet utama dari PC Host.

### Mode 1: Absolute Transparent L2 Bridge (Fitur Utama)

Mode ini membuat perangkat Wi-Fi kamu benar-benar tembus ke jaringan LAN utama. Butuh akses Administrator (atau root) untuk menjalankan script-nya.
* LwIP DHCP Server dimatikan seketika untuk mencegah bentrok IP (Rogue DHCP) di jaringan asli.
* Mendukung penuh: DHCP Passthrough (dapat IP langsung dari router asli), ARP, IPv4 Unicast/Broadcast, Multicast, IPv6, TCP, dan ICMP.

### Mode 2: Enterprise Stealth Bridge

Mode khusus untuk beroperasi di jaringan dengan security yang ketat.
* Bypass Switch Port Security (MAC NAT): Switch enterprise biasanya memblokir port jika ada lebih dari 1 MAC address. Mode ini mengubah (spoofing) MAC address asal semua paket menjadi satu MAC tunggal milik USB.
* Bypass WIPS (Wireless Intrusion Prevention System): Menyembunyikan SSID, menurunkan daya pancar Wi-Fi (menjadi sekitar 8.5 dBm agar tidak terdeteksi sensor plafon), dan mewajibkan fitur PMF (Protected Management Frames) agar tidak bisa diserang menggunakan deauthentication.
* Bypass USB Endpoint DLP: Fitur Flash Disk (Mass Storage) dimatikan secara otomatis agar tidak diblokir oleh antivirus atau sistem keamanan. Alat hanya akan terbaca sebagai USB Network Adapter biasa.
* Peringatan: Akses LAN pada mode ini mungkin saja terblokir jika PC Host sedang menjalankan VPN dengan kebijakan "Split-Tunnel" yang mengunci routing lokal.

## Arsitektur & Teknologi (Tech Stack)

Proyek ini menggunakan bahasa C murni (tanpa framework Arduino) dan Go.

* Firmware Core: C11 menggunakan ESP-IDF v6.1.
* Host Proxy App: Golang 1.22+. Ditulis hanya menggunakan Standard Library (net, io, net/http) agar bisa dikompilasi offline tanpa konflik dependensi.
* USB Stack (usb_composite.c): TinyUSB. Berfungsi sebagai perangkat gabungan CDC-NCM dan Mass Storage (MSC).
* Optimalisasi Multi-Core: Pada ESP32-S3, tugas jaringan berat (USB & L2 TX) dipaku di Core 1, sedangkan Wi-Fi, Web GUI, dan Layar OLED di Core 0. Untuk ESP32-S2 (Single-Core), semuanya menggunakan alokasi dinamis.
* Sistem File Self-Healing (fs_healing.c): Jika ada file di flash disk yang korup, sengaja diformat (bukan FAT12), atau terhapus, ESP32 akan mengekstrak ulang file-file tersebut dari partisi rodata secara otomatis dalam hitungan milidetik. Kapasitas penyimpanan untuk S3 adalah 10 MB, sedangkan S2 adalah 2.4 MB.
* Protokol Kontrol UDP (Port 33333): Script dari komputer mengontrol ESP32 melalui paket UDP ke port 33333, sehingga tidak perlu mencari-cari nomor COM Port yang sering berubah.

### Struktur Direktori Proyek

Seluruh file wajib mengikuti struktur ini secara persis:
* CMakeLists.txt & sdkconfig.defaults (termasuk .esp32s3 dan .esp32s2)
* partitions_s3.csv & partitions_s2.csv
* main/ (Berisi source code C utama: main, app_state, fs_healing, usb_composite, wifi_bridge, mac_nat, ui_oled, button_input, web_gui)
* components/rodata_assets/ (Berisi HTML dan file script host seperti Start_Bridge.bat, config.txt)
* tools/host_proxy/ (Berisi source code Go proxy)

## Cara Membuat dan Merakit Alat (DIY Guide)

### 1. Kebutuhan Perangkat Keras

* Board Mikrokontroler: ESP32-S3 N16R8 (Disarankan, Flash 16MB) atau ESP32-S2 Supermini (Flash 4MB).
* Layar: OLED I2C 128x64 (chip SSD1306).
* Tombol fisik (opsional, bawaan board sudah cukup untuk 1 tombol).

### 2. Skema Kabel (Wiring GPIO)

ESP32-S3 (N16R8):
* Layar OLED: SDA = 8, SCL = 9.
* Tombol Select: GPIO 0 (Tombol BOOT). Tombol Up (Opsional): GPIO 4. Tombol Down (Opsional): GPIO 5.

ESP32-S2 (Supermini):
* Layar OLED: SDA = 33, SCL = 35.
* Tombol Select: GPIO 0 (Tombol BOOT). Tombol Up (Opsional): GPIO 1. Tombol Down (Opsional): GPIO 2.

### 3. Kompilasi dan Flashing

1. Gunakan ESP-IDF v6.1. Buka terminal ESP-IDF.
2. Atur target: idf.py set-target esp32s3 (atau esp32s2).
3. Atur jumlah tombol: idf.py menuconfig -> masuk ke menu "Maryati Bridge Configuration" -> Atur "Button Scheme".
4. Flash ke alat: idf.py build flash monitor

## Cara Penggunaan

### 1. Menyalakan Alat

Colokkan Maryati Bridge ke port USB PC/Laptop. Alat otomatis masuk ke Mode 0 dan layarnya menampilkan SSID serta Password acak (tanpa karakter 0, O, 1, l, I). Hubungkan gadget kamu ke Wi-Fi tersebut.

### 2. Kontrol Tombol Fisik (OLED Navigasi)

Alat ini punya fitur screensaver (layar meredup di menit ke-2, mati di menit ke-5). Tekan sembarang tombol untuk membangunkan layar. Fungsi tombol (setelah layar menyala):

* Skema 1 Tombol (Hanya pakai tombol BOOT)
  * Tekan Singkat (< 0.5 detik): Menggeser pilihan mode.
  * Tekan Lama (2-5 detik): Menerapkan mode.
  * Tahan (>= 10 detik): Factory Reset.

* Skema 2 Tombol (NEXT + SELECT/BOOT)
  * NEXT: Menggeser mode.
  * SELECT: Menerapkan mode.
  * Tahan Keduanya (5 detik): Factory Reset.

* Skema 3 Tombol (UP, DOWN, SELECT/BOOT)
  * UP/DOWN: Navigasi menu.
  * SELECT: Menerapkan mode.
  * Tekan UP+DOWN Bersamaan: Langsung putuskan koneksi klien dan buat kredensial Wi-Fi baru saat itu juga.
  * Tahan UP+SELECT (5 detik): Factory Reset.

* Tombol RESET/EN Board: Berfungsi sebagai "Panic Button". Akan me-restart alat secara kilat dan mengganti password (jika tidak pakai konfigurasi statis).

### 3. Penggunaan Script via USB Flash Drive

Saat dicolokkan, akan muncul partisi bernama "MARYATI".
* Start_Proxy: Jalankan (tanpa admin) untuk akses proxy di Mode 0.
* Start_Bridge: Jalankan (Run as Administrator/root) untuk menembus LAN langsung (Mode 1).
* Start_Stealth: Jalankan untuk mengaktifkan lapisan MAC NAT di jaringan ketat (Mode 2).

### 4. Web GUI dan API

Akses dasbor dari browser: http://192.168.33.1 (Login menggunakan password Wi-Fi saat ini).
Jika kamu ingin membuat otomatisasi, alat ini menyediakan REST API (butuh header otorisasi):
* GET /api/status: Menampilkan status, statistik TX/RX, dan penggunaan RAM.
* POST /api/mode: Mengganti mode aktif.
* POST /api/config: Mengganti pengaturan permanen.
* POST /api/action: Memanggil perintah seperti rotasi password atau reset alat.

### 5. Konfigurasi Permanen (config.txt)

Buka drive "MARYATI" dan edit file config.txt. Ubah use_static=1, isi static_ssid dan static_pass, lalu simpan. Kamu juga bisa mengubah nilai MAC address palsu, VID/PID USB, dan menyembunyikan SSID dari file ini.

## Cheat Sheet

Tahap Pembuatan:
1. Siapkan ESP32-S3 (SDA=8, SCL=9) atau S2 (SDA=33, SCL=35) & Layar OLED SSD1306.
2. Gunakan ESP-IDF v6.1.
3. idf.py set-target esp32s3 -> idf.py menuconfig -> idf.py build flash monitor.

Pemakaian:
* Mode Proxy (Mode 0): Colok USB -> Konek Wi-Fi -> Buka drive MARYATI -> Jalankan Start_Proxy.bat.
* Mode Bridge (Mode 1): Colok USB -> Konek Wi-Fi -> Buka drive MARYATI -> Klik kanan Start_Bridge.bat -> Run as Administrator.
* Mode Stealth (Mode 2): Colok USB -> Konek Wi-Fi -> Buka drive MARYATI -> Jalankan Start_Stealth.bat.
* Web Admin: Buka http://192.168.33.1 di browser.
* Factory Reset: Tekan tombol fisik sesuai skema, atau lewat Web Admin, atau format flash disk lewat PC (file akan dipulihkan otomatis).
