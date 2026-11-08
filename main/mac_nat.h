#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "app_state.h"

/* Pemetaan IPv4 klien Wi-Fi -> MAC asli klien. Entri kedaluwarsa setelah
 * tidak ada aktivitas selama 5 menit. */
#define MB_MAC_NAT_TTL_SEC          300

typedef struct {
    uint32_t client_ip;              /* IPv4 host byte order */
    uint8_t  client_mac[6];
    uint32_t last_seen_sec;          /* esp_timer seconds */
    bool     valid;
} mac_nat_entry_t;

/* Inisialisasi tabel + mutex. Aman dipanggil sekali. */
esp_err_t mac_nat_init(void);

/* Frame Wi-Fi -> USB (Mode 2).
 * Mencatat pasangan [source IPv4 / ARP SPA, MAC asli] lalu menulis ulang
 * Ethernet source MAC (dan ARP SHA) menjadi spoof_mac.
 * Mengembalikan ESP_OK bila frame dimodifikasi, ESP_ERR_NOT_FOUND bila bukan
 * IPv4/ARP, ESP_ERR_NOT_SUPPORTED bila frame tidak bisa diterjemahkan. */
esp_err_t mac_nat_translate_tx(uint8_t *eth_frame, uint16_t len,
                               const uint8_t spoof_mac[6]);

/* Frame USB -> Wi-Fi (Mode 2).
 * Bila IPv4 unicast / ARP menuju IP yang ada di tabel, Ethernet destination
 * MAC (dan ARP THA) dikembalikan ke MAC asli klien. */
esp_err_t mac_nat_translate_rx(uint8_t *eth_frame, uint16_t len);

/* Hapus seluruh entri (dipakai saat keluar/masuk Mode 2). */
void mac_nat_clear(void);
