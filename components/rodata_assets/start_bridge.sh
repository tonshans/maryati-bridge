#!/usr/bin/env bash
# ---------------------------------------------------------------
#  start_bridge.sh - Mode 1: Absolute Transparent L2 Bridge
#  Membuat bridge br_maryati (LAN + USB Maryati), memindahkan IP
#  LAN ke bridge, lalu mengirim CMD:MODE=1:<LAN_IP> ke ESP32.
#  Jalankan: sudo ./start_bridge.sh
# ---------------------------------------------------------------
set -euo pipefail

BR_IF="br_maryati"
ESP_IP="192.168.33.1"
ESP_PORT="33333"

if [[ ${EUID} -ne 0 ]]; then
    echo "[!] Jalankan sebagai root: sudo $0"
    exit 1
fi

LAN_IF=$(ip -4 route show default | awk 'NR==1 {print $5}')
if [[ -z ${LAN_IF} ]]; then
    echo "[!] Tidak ada interface LAN dengan default route."
    exit 1
fi

LAN_CIDR=$(ip -4 -o addr show dev "${LAN_IF}" scope global | awk 'NR==1 {print $4}')
LAN_IP=${LAN_CIDR%%/*}
GW=$(ip -4 route show default | awk 'NR==1 {print $3}')
if [[ -z ${LAN_CIDR} || -z ${GW} ]]; then
    echo "[!] ${LAN_IF} tidak punya IPv4/default gateway."
    exit 1
fi

# Adapter USB Maryati: MAC prefix 0B:DA:81 (VID/PID tinyusb), fallback driver cdc_ncm.
USB_IF=""
for n in /sys/class/net/*; do
    name=$(basename "${n}")
    mac=$(cat "${n}/address" 2>/dev/null || true)
    if [[ ${mac} == 0b:da:81:* ]]; then USB_IF=${name}; break; fi
done
if [[ -z ${USB_IF} ]]; then
    for n in /sys/class/net/*; do
        name=$(basename "${n}")
        drv=$(basename "$(readlink -f "${n}/device/driver" 2>/dev/null)" 2>/dev/null || true)
        if [[ ${drv} == cdc_ncm || ${drv} == cdc_ether ]]; then USB_IF=${name}; break; fi
    done
fi
if [[ -z ${USB_IF} ]]; then
    echo "[!] Interface USB Maryati tidak ditemukan (cek kabel / driver cdc_ncm)."
    exit 1
fi

echo "[*] LAN : ${LAN_IF} (${LAN_CIDR}, gw ${GW})"
echo "[*] USB : ${USB_IF}"

# Kirim perintah dulu, selagi USB masih punya IP 192.168.33.2 dari DHCP Mode 0.
echo "[*] Mengirim CMD:MODE=1:${LAN_IP} ke ${ESP_IP}:${ESP_PORT} ..."
printf 'CMD:MODE=1:%s' "${LAN_IP}" > "/dev/udp/${ESP_IP}/${ESP_PORT}" || \
    echo "[!] Paket kontrol gagal dikirim; lanjut membuat bridge."

echo "[*] Membuat bridge ${BR_IF} ..."
ip link add name "${BR_IF}" type bridge 2>/dev/null || true
ip link set "${BR_IF}" type bridge stp_state 0 forward_delay 0 ageing_time 300
ip link set "${BR_IF}" up

# Pindahkan IP LAN ke bridge (bridge yang memegang alamat, bukan slave).
ip addr flush dev "${LAN_IF}"
ip link set "${LAN_IF}" master "${BR_IF}"
ip addr flush dev "${USB_IF}" 2>/dev/null || true
ip link set "${USB_IF}" master "${BR_IF}"

ip addr add "${LAN_CIDR}" dev "${BR_IF}"
ip route replace default via "${GW}" dev "${BR_IF}"

echo "[+] Bridge aktif: ${BR_IF} = ${LAN_IF} + ${USB_IF} (${LAN_CIDR})"
echo "    Klien Wi-Fi kini memperoleh DHCP langsung dari router LAN."
echo "    Untuk membongkar: sudo ip link del ${BR_IF}"
