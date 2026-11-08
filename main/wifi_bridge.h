#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "app_state.h"

/* Nyalakan SoftAP, daftarkan RX callback L2, lalu terapkan mode awal.
 * Wajib dipanggil setelah mac_nat_init(). */
esp_err_t wifi_bridge_init(void);

/* Frame Ethernet mentah dari PC Host (CDC-NCM). Menangani paket kontrol UDP
 * 33333, MAC NAT RX (Mode 2), lalu meneruskan ke udara. */
esp_err_t wifi_bridge_from_usb(const uint8_t *frame, uint16_t len);

/* Terapkan mode aktif: hidden SSID/PMF/TX power + DHCP server (Mode 0 saja). */
esp_err_t wifi_bridge_apply_mode(app_mode_t mode);

/* Ganti SSID/password SoftAP aktif (rotate credentials). */
esp_err_t wifi_bridge_apply_credentials(const char *ssid, const char *pass);
