# Maryati Bridge

Maryati Bridge adalah alat diagnostik jaringan dan jembatan Layer-2 (Transparent USB-to-Wi-Fi LAN Bridge) berbasis ESP32-S3 atau ESP32-S2. Alat ini dirancang untuk menjembatani perangkat Wi-Fi kamu (seperti laptop teknisi, PC kedua, atau smartphone) agar bisa masuk ke dalam jaringan LAN kabel milik sebuah PC Host lewat port USB.

Rasanya persis seperti kamu mencolokkan kabel LAN langsung ke switch atau router fisik, tapi semuanya dilakukan secara nirkabel dari sisi klien.

Alat ini sangat cocok untuk Network Engineer, Pentester, teknisi lapangan, atau sekadar pengguna biasa yang ingin main game LAN dan berbagi file tanpa ribet konfigurasi jaringan.

## Fitur Utama (Tri-Mode Adaptive)

Sistem operasi modern biasanya memblokir jaringan USB baru dengan alasan keamanan. Untuk mengatasinya, Maryati Bridge punya 3 mode pintar yang bisa disesuaikan dengan kondisimu:

### Mode 0: P2P Fallback & Local Proxy (Mode Bawaan)

Mode ini otomatis menyala saat alat dicolokkan. Sangat berguna kalau kamu tidak punya akses Administrator di PC Host.

* Membuat jaringan lokal sendiri (subnet 192.168.33.x) antara perangkat Wi-Fi dan PC Host.
* Tidak mengganggu koneksi internet atau jaringan asli di PC Host.
* Terdapat fitur Portable Proxy di dalam USB Flash Drive-nya. Kamu bisa menjalankan aplikasi proxy ini (tanpa perlu hak admin) untuk ikut menggunakan internet atau akses LAN dari PC Host.

### Mode 1: Absolute Transparent L2 Bridge (Fitur Utama)

Mode ini membuat perangkat Wi-Fi kamu benar-benar tembus ke jaringan LAN utama secara transparan. Butuh akses Administrator untuk menjalankannya.

* Mendukung DHCP Passthrough (dapat IP langsung dari router asli), ARP, Broadcast, Multicast, hingga IPv6.
* PC Host akan bertindak sebagai jembatan tak terlihat antara jaringan Wi-Fi Maryati dan jaringan kabel.

### Mode 2: Enterprise Stealth Bridge (Pertahanan Ekstra)

Mode khusus untuk beroperasi di jaringan kantor yang sangat ketat atau untuk kebutuhan penetration testing.

* Bypass Switch Port Security: Melakukan MAC NAT (spoofing) sehingga switch hanya melihat 1 MAC address (tidak akan memicu pemblokiran port).
* Bypass WIPS (Wireless Intrusion Prevention System): Menyembunyikan SSID, menurunkan daya pancar Wi-Fi agar tidak terdeteksi sensor plafon, dan menolak serangan deauthentication (karena menggunakan PMF).
* Bypass USB DLP: Mematikan fungsi penyimpanan flash disk (Mass Storage) secara otomatis, sehingga alat ini hanya terbaca sebagai USB Network Adapter murni dan tidak diblokir oleh antivirus atau sistem keamanan PC kantor.

### Fitur Keren Lainnya

* Layar OLED 128x64: Menampilkan informasi penting seperti Mode, SSID, Password, dan IP Host. Layar ini punya fitur anti burn-in (akan meredup dan mati sendiri kalau tidak ada aktivitas).
* Kredensial Acak Aman: Setiap kali dinyalakan, SSID dan Password dibuat secara acak. Karakter yang membingungkan seperti '0', 'O', '1', 'l', 'I' sudah dihilangkan agar mudah dibaca di layar.
* Self-Healing Flash Drive: File script dan proxy tersimpan aman di dalam sistem. Kalau kamu tidak sengaja menghapus file atau flash disk minta diformat, sistem akan memulihkannya secara otomatis dalam hitungan milidetik.
* Web GUI: Bisa diakses dari browser untuk memantau status jaringan dan ganti pengaturan tanpa perlu pegang alat fisiknya.

## Cara Membuat dan Merakit Alat (DIY Guide)

Berikut adalah tahapan dari merakit hingga mengisi firmware.

### 1. Kebutuhan Perangkat Keras
* Board Mikrokontroler: ESP32-S3 N16R8 (Sangat disarankan, Flash 16MB) atau ESP32-S2 Supermini (Flash 4MB).
* Layar: OLED I2C ukuran 128x64 (chip SSD1306).
* Komponen lain: Kabel jumper, breadboard (untuk prototype), dan tombol push-button (opsional jika kamu ingin pakai lebih dari 1 tombol).

### 2. Skema Kabel (Wiring GPIO)
Sambungkan pin-pin berikut sesuai dengan jenis chip yang kamu gunakan:

Untuk ESP32-S3 (N16R8):
* Layar OLED: Pin SDA ke GPIO 8, Pin SCL ke GPIO 9. VCC ke 3.3V dan GND ke GND.
* Tombol Select: Bisa menggunakan tombol BOOT bawaan (GPIO 0).
* Tombol Up (Opsional): GPIO 4.
* Tombol Down (Opsional): GPIO 5.

Untuk ESP32-S2 (Supermini):
* Layar OLED: Pin SDA ke GPIO 33, Pin SCL ke GPIO 35. VCC ke 3.3V dan GND ke GND.
* Tombol Select: Bisa menggunakan tombol BOOT bawaan (GPIO 0).
* Tombol Up (Opsional): GPIO 1.
* Tombol Down (Opsional): GPIO 2.

### 3. Persiapan Software dan Kompilasi
Firmware ini ditulis menggunakan bahasa C asli. Kamu WAJIB menggunakan ESP-IDF versi 6.1 (jangan menggunakan Arduino IDE).

1. Install ESP-IDF v6.1 di komputermu.
2. Buka terminal (atau ESP-IDF Command Prompt), lalu clone repositori ini dan masuk ke foldernya.
3. Tentukan target mikrokontroler dengan perintah:
   idf.py set-target esp32s3
   (Ganti menjadi esp32s2 jika kamu menggunakan S2. Perintah ini otomatis mengatur konfigurasi memori dan partisi yang sesuai).
4. Masuk ke menu konfigurasi:
   idf.py menuconfig
5. Cari menu "Maryati Bridge Configuration". Di dalam sana, kamu bisa mengatur "Button Scheme". Silakan pilih opsi 1 (hanya 1 tombol bawaan), opsi 2, atau opsi 3 tombol sesuai dengan jumlah tombol yang kamu rakit.
6. Keluar dari menuconfig (tekan Q dan simpan).

### 4. Flashing ke Perangkat
Sambungkan ESP32 kamu ke komputer menggunakan kabel USB data, lalu jalankan perintah:
idf.py build flash monitor

Tunggu sampai proses selesai. Perintah "monitor" akan menampilkan log dari alat untuk memastikan tidak ada error saat alat pertama kali menyala.

### 5. Tahap Uji Coba Awal (Testing)
1. Setelah flashing selesai, lihat ke layar OLED. Layar seharusnya menyala dan menampilkan Mode 0 beserta SSID dan Password acak.
2. Buka pengaturan jaringan komputer/PC Host, kamu akan melihat perangkat USB baru (biasanya terdeteksi sebagai Realtek USB GbE Family Controller atau semacamnya).
3. Buka File Explorer di PC Host. Kamu seharusnya melihat drive penyimpanan baru (Flash Disk) bernama "MARYATI" yang berisi file-file script dan folder proxy.
4. Coba hubungkan handphone atau laptop keduamu ke Wi-Fi Maryati menggunakan kredensial yang ada di layar.

Jika semua tahapan di atas berhasil, alatmu sudah siap digunakan sepenuhnya!

## Cara Penggunaan

### 1. Memulai (Booting)
1. Colokkan Maryati Bridge ke port USB PC/Laptop Host.
2. Lihat ke layar OLED. Alat akan otomatis berada di Mode 0. Layar akan menampilkan SSID dan Password Wi-Fi acak.
3. Hubungkan laptop atau gadget kamu ke Wi-Fi tersebut.

### 2. Mengganti Mode via USB Flash Drive
Saat kamu mencolokkan alat ini, akan muncul drive penyimpanan baru bernama "MARYATI" di PC Host. Di dalamnya ada beberapa script yang bisa kamu jalankan sesuai kebutuhan:

* Start_Proxy: Jalankan ini (klik ganda) kalau kamu mau pakai proxy di Mode 0 tanpa hak admin.
* Start_Bridge: Jalankan ini sebagai Administrator (Run as Administrator) di Windows atau gunakan sudo di Linux untuk masuk ke Mode 1 (Transparent Bridge).
* Start_Stealth: Jalankan ini untuk masuk ke Mode 2 yang super aman.

Script ini akan otomatis memberi tahu alat untuk berganti mode, dan layar OLED akan langsung memperbarui statusnya.

### 3. Menggunakan Web GUI
Kalau kamu sedang terhubung ke Wi-Fi Maryati, kamu bisa membuka browser dan mengetik alamat:
http://192.168.33.1

* Akan muncul permintaan login. Masukkan password yang sama persis dengan password Wi-Fi yang tampil di layar OLED.
* Di dashboard ini, kamu bisa melihat statistik jaringan, mengganti mode dengan sekali klik, atau merotasi password Wi-Fi.

### 4. Menggunakan Tombol Fisik
Tergantung varian hardware yang kamu rakit (1, 2, atau 3 tombol), kamu bisa menekan tombol untuk:

* Navigasi antar mode langsung dari layar OLED.
* Menahan tombol untuk menerapkan mode.
* Membangunkan layar OLED yang sedang tertidur.
* Melakukan Factory Reset (menahan tombol tertentu selama beberapa detik) untuk mengembalikan alat ke pengaturan pabrik. Tombol RESET fisik juga bisa dipakai untuk langsung mengganti SSID dan password secara instan.

### 5. Konfigurasi Permanen (config.txt)
Kalau kamu bosan dengan password yang berubah-ubah setiap saat, buka drive "MARYATI" dan edit file config.txt.
Kamu bisa mengubah pengaturan use_static=1, lalu menentukan static_ssid dan static_pass sesuka hati kamu. Setelah disimpan, cabut dan colok kembali alatnya untuk menerapkan pengaturan baru. Di file ini kamu juga bisa mengatur mode bawaan saat pertama kali menyala, mengganti MAC address tiruan, dan lain-lain.
