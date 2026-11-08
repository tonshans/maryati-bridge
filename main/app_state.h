#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define MB_SSID_MAX_LEN       32
#define MB_PASS_MAX_LEN       64
#define MB_IP_STR_MAX_LEN     16
#define MB_MAX_WIFI_CLIENTS   4
#define MB_MAC_NAT_MAX_ENTRIES 16

typedef enum {
    MODE_P2P_PROXY = 0,          // Default Fallback: DHCP Server ON (No Gateway) + L2 local + Host Proxy
    MODE_TRANSPARENT = 1,        // Main Feature: DHCP Server OFF + Pure L2 Bridge (USB <-> Wi-Fi)
    MODE_ENTERPRISE_STEALTH = 2  // Corporate Defense: L2 Bridge + MAC NAT + Hidden SSID + Low TX + PMF
} app_mode_t;

typedef enum {
    OLED_STATE_ACTIVE = 0,
    OLED_STATE_DIMMED = 1,       // Setelah 120 detik tanpa input tombol
    OLED_STATE_OFF = 2           // Setelah 300 detik tanpa input tombol
} oled_power_state_t;

typedef struct {
    bool use_static_cred;                // 0 = Random Base58 setiap boot, 1 = Gunakan static_ssid/pass
    char static_ssid[MB_SSID_MAX_LEN];
    char static_pass[MB_PASS_MAX_LEN];
    app_mode_t default_boot_mode;        // Default: MODE_P2P_PROXY (0)
    bool enable_msc;                     // true = Tampilkan USB Flash Drive, false = Stealth (Hanya NCM)
    bool hidden_ssid;                    // true = Sembunyikan SSID (otomatis true pada Mode 2)
    int8_t tx_power_dbm;                 // Default: 20 dBm (Mode 0/1), otomatis 8 dBm pada Mode 2
    uint8_t custom_mac[6];               // Digunakan untuk MAC Spoofing / Masquerading di Mode 2
    uint16_t usb_vid;                    // Default: 0x0BDA (Realtek)
    uint16_t usb_pid;                    // Default: 0x8152
} sys_config_t;

typedef struct {
    char active_ssid[MB_SSID_MAX_LEN];
    char active_password[MB_PASS_MAX_LEN];
    app_mode_t current_mode;
    app_mode_t preview_menu_mode;        // Untuk navigasi tombol di OLED sebelum dikonfirmasi
    bool menu_selection_active;
    uint8_t connected_clients;
    oled_power_state_t oled_state;
    uint32_t last_input_timestamp_sec;
    bool proxy_active_on_host;
    char host_ip[MB_IP_STR_MAX_LEN];     // Default "192.168.33.2", diperbarui saat script host lapor IP LAN
    uint64_t usb_rx_frames;
    uint64_t usb_tx_frames;
    uint64_t wifi_rx_frames;
    uint64_t wifi_tx_frames;
} app_state_t;

// API Manajemen State Thread-Safe (dengan FreeRTOS Mutex)
esp_err_t app_state_init(void);
void app_state_get(app_state_t *out_state);
void app_state_get_config(sys_config_t *out_cfg);
esp_err_t app_state_set_mode(app_mode_t new_mode);
esp_err_t app_state_update_config(const sys_config_t *new_cfg, bool save_to_nvs);
void app_state_generate_random_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);
void app_state_record_button_activity(void);
void app_state_set_host_info(const char *ip_str, bool proxy_on);
void app_state_inc_counters(uint32_t usb_rx, uint32_t usb_tx, uint32_t wifi_rx, uint32_t wifi_tx);

// Tambahan untuk modul UI / bridge / web (tidak mengubah kontrak di atas).
void app_state_set_connected_clients(uint8_t count);
void app_state_set_oled_state(oled_power_state_t state);
void app_state_set_preview_menu(app_mode_t preview, bool selection_active);
esp_err_t app_state_set_credentials(const char *ssid, const char *pass);
