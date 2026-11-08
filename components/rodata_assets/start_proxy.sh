#!/usr/bin/env bash
# ---------------------------------------------------------------
#  start_proxy.sh - Portable proxy tanpa root (Mode 0)
#  HTTP/HTTPS CONNECT : 192.168.33.2:8080
#  SOCKS5 (TCP/UDP)   : 192.168.33.2:1080
# ---------------------------------------------------------------
set -euo pipefail

cd "$(dirname "$0")"

if [[ ! -x ./proxy_linux ]]; then
    if [[ -f ./proxy_linux ]]; then
        chmod +x ./proxy_linux
    else
        echo "[!] proxy_linux tidak ada di folder ini."
        echo "    Build dengan tools/host_proxy/build_all.sh lalu salin hasilnya."
        exit 1
    fi
fi

echo "[*] Menjalankan proxy_linux (tanpa root)..."
exec ./proxy_linux
