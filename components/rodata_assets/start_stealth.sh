#!/usr/bin/env bash
# ---------------------------------------------------------------
#  start_stealth.sh - Mode 2: Enterprise Stealth Bridge
#  Sama seperti start_bridge.sh, tetapi mengirim CMD:MODE=2.
#  ESP32 akan: hidden SSID, TX ~8 dBm, PMF wajib, MAC NAT.
#  Set enable_msc=0 pada config.txt untuk menyembunyikan drive USB.
#  Jalankan: sudo ./start_stealth.sh
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

echo "[*] Mengirim CMD:MODE=2:${LAN_IP} ke ${ESP_IP}:${ESP_PORT} ..."
printf 'CMD:MODE=2:%s' "${LAN_IP}" > "/dev/udp/${ESP_IP}/${ESP_PORT}" || \
    echo "[!] Paket kontrol gagal dikirim; lanjut membuat bridge."

echo "[*] Membuat bridge ${BR_IF} ..."
ip link add name "${BR_IF}" type bridge 2>/dev/null || true
ip link set "${BR_IF}" type bridge stp_state 0 forward_delay 0 ageing_time 300
ip link set "${BR_IF}" up

ip addr flush dev "${LAN_IF}"
ip link set "${LAN_IF}" master "${BR_IF}"
ip addr flush dev "${USB_IF}" 2>/dev/null || true
ip link set "${USB_IF}" master "${BR_IF}"

ip addr add "${LAN_CIDR}" dev "${BR_IF}"
ip route replace default via "${GW}" dev "${BR_IF}"

echo "[+] Mode 2 aktif: ${BR_IF} = ${LAN_IF} + ${USB_IF} (${LAN_CIDR})"
echo "    SSID hidden, TX ~8 dBm, PMF required, MAC klien di-masquerade."
echo "    Untuk membongkar: sudo ip link del ${BR_IF}"
