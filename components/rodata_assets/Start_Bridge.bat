@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Maryati Bridge - Mode 1 Transparent L2 Bridge (Admin)

rem ---------------------------------------------------------------
rem  Start_Bridge.bat
rem  Menghubungkan adapter LAN aktif dengan adapter USB Maryati
rem  Bridge memakai Windows Network Bridge, lalu mengirim perintah
rem  CMD:MODE=1:<LAN_IP> ke ESP32 (192.168.33.1:33333).
rem ---------------------------------------------------------------

net session >nul 2>&1
if errorlevel 1 (
    echo [*] Meminta hak Administrator...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

set "ESP_IP=192.168.33.1"
set "ESP_PORT=33333"

rem --- Adapter USB Maryati (CDC-NCM, VID 0BDA / PID 8152) ---
set "USB_IF="
for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "(Get-NetAdapter -IncludeHidden | Where-Object { $_.InterfaceDescription -match 'Maryati|NCM' -or $_.MacAddress -like '0B-DA-81-52-*' } | Select-Object -First 1 -ExpandProperty Name)"`) do set "USB_IF=%%A"
if not defined USB_IF (
    echo [!] Adapter USB Maryati tidak ditemukan.
    echo     Pastikan kabel USB tersambung dan driver CDC-NCM terpasang.
    pause
    exit /b 1
)

rem --- Adapter LAN dengan default route aktif ---
set "LAN_IF="
for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "(Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object RouteMetric | Select-Object -First 1).InterfaceAlias"`) do set "LAN_IF=%%A"
if not defined LAN_IF (
    echo [!] Tidak ada adapter LAN dengan default route.
    pause
    exit /b 1
)

set "LAN_IP="
for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "(Get-NetIPAddress -InterfaceAlias '%LAN_IF%' -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1).IPAddress"`) do set "LAN_IP=%%A"
if not defined LAN_IP (
    echo [!] Adapter LAN '%LAN_IF%' tidak punya alamat IPv4.
    pause
    exit /b 1
)

echo [*] LAN : %LAN_IF%  (%LAN_IP%)
echo [*] USB : %USB_IF%
echo [*] Mengirim CMD:MODE=1:%LAN_IP% ke %ESP_IP%:%ESP_PORT% ...
powershell -NoProfile -Command "$u = New-Object System.Net.Sockets.UdpClient; $b = [Text.Encoding]::ASCII.GetBytes('CMD:MODE=1:%LAN_IP%'); [void]$u.Send($b, $b.Length, '%ESP_IP%', %ESP_PORT%); $u.Close()"
if errorlevel 1 echo [!] Paket kontrol gagal dikirim (firewall?). Lanjut membuat bridge.

echo [*] Membuat Network Bridge (L2)...
netsh bridge install >nul 2>&1
netsh bridge add adapter "%LAN_IF%" >nul 2>&1
netsh bridge add adapter "%USB_IF%" >nul 2>&1
netsh bridge show adapter

echo.
echo [+] Bridge aktif. Klien Wi-Fi Maryati sekarang berada di LAN %LAN_IP%/24.
echo     Catatan: Windows memindahkan IP/DHCP ke bridge, adapter LAN bisa
echo     kehilangan IP sesaat. Jika gagal, buat bridge manual di
echo     Network Connections: pilih adapter LAN + USB -^> Bridge Connections.
echo.
pause
