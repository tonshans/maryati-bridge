# Prebuilt Host Proxy

Binary siap pakai untuk PC Host. Tidak perlu compile Go.

| File | Platform | Port |
|------|----------|------|
| `proxy_win.exe` | Windows x86-64 | HTTP 8080, SOCKS5 1080 |
| `proxy_linux` | Linux x86-64 | HTTP 8080, SOCKS5 1080 |

Keduanya statis (`CGO_ENABLED=0`), tanpa dependency eksternal, dan berjalan tanpa hak Administrator/root.

## Pakai

1. Salin binary sesuai OS ke root drive Maryati Bridge (drive USB MSC).
2. Windows: jalankan `Start_Proxy.bat`.
   Linux: jalankan `./start_proxy.sh`.
3. Proxy mengirim `CMD:PROXY_ON:<IP_LAN_HOST>` ke `192.168.33.1:33333`; OLED menampilkan IP host.

## Recovery Setelah FAT Corrupt / Factory Reset

Firmware memformat ulang partisi FAT dan memulihkan script/config dari `.rodata`, tetapi **tidak** memulihkan binary proxy (ukurannya melebihi partisi aplikasi). Setelah reset:

1. Unduh `proxy_win.exe` atau `proxy_linux` dari folder ini.
2. Salin ke root drive Maryati.
3. Jalankan `Start_Proxy.bat` / `start_proxy.sh` lagi.

Simpan salinan binary di PC agar recovery tetap bisa dilakukan tanpa internet.

## Verifikasi

```bash
cd prebuilt
sha256sum -c SHA256SUMS
```

Windows (PowerShell):

```powershell
Get-FileHash .\proxy_win.exe -Algorithm SHA256
```

## Build Ulang

```bash
cd tools/host_proxy
./build_all.sh
```

Script menulis ulang kedua binary dan `SHA256SUMS` di folder ini.

## Catatan

- ESP32-S2 punya partisi storage ~2.43 MB, lebih kecil dari ukuran binary (~6 MB). Binary tidak muat di drive S2; gunakan S3 atau jalankan proxy langsung dari PC.
- Binary Go tidak ditandatangani. Windows SmartScreen/antivirus bisa menampilkan peringatan.
- Target yang dibangun: `windows/amd64` dan `linux/amd64`.
