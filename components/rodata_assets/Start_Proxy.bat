@echo off
setlocal EnableExtensions
title Maryati Bridge - Portable Proxy (tanpa Admin)

rem ---------------------------------------------------------------
rem  Start_Proxy.bat
rem  Menjalankan proxy_win.exe dari drive Maryati (tanpa hak Admin).
rem  HTTP/HTTPS CONNECT : 192.168.33.2:8080
rem  SOCKS5 (TCP/UDP)   : 192.168.33.2:1080
rem ---------------------------------------------------------------

cd /d "%~dp0"

if not exist "proxy_win.exe" (
    echo [!] proxy_win.exe tidak ada di folder ini.
    echo     Jalankan build_all.sh di tools/host_proxy lalu salin hasilnya.
    pause
    exit /b 1
)

echo [*] Menjalankan proxy_win.exe ...
start "" /b "%~dp0proxy_win.exe"

echo.
echo [+] Proxy berjalan di PC Host:
echo     HTTP/HTTPS : 192.168.33.2:8080
echo     SOCKS5     : 192.168.33.2:1080
echo.
echo     Arahkan browser/aplikasi ke salah satu proxy di atas.
echo     Jika muncul Windows Firewall, izinkan akses untuk Private Network.
echo     Tutup jendela ini tidak mematikan proxy; hentikan lewat Task Manager.
echo.
pause
