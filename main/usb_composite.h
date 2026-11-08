#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/* Nyalakan TinyUSB CDC-NCM sesuai sys_config_t (VID/PID + enable_msc).
 * Wajib dipanggil setelah wifi_bridge_init() agar jalur data siap. */
esp_err_t usb_composite_init(void);

/* Kirim satu frame Ethernet mentah ke PC Host lewat CDC-NCM.
 * Thread-safe; buffer boleh berasal dari stack (disalin oleh TinyUSB). */
esp_err_t usb_composite_send_eth_frame(const uint8_t *frame, uint16_t len);

/* Tulis file ke partisi FAT yang dipegang MSC (untuk config.txt).
 * Storage sementara dipasang ke aplikasi, lalu dikembalikan ke USB host.
 * Mengembalikan ESP_ERR_INVALID_STATE bila MSC tidak aktif. */
esp_err_t usb_composite_msc_write_file(const char *name, const void *data, size_t len);

/* True bila storage MSC benar-benar aktif (enable_msc dan init sukses). */
bool usb_composite_msc_active(void);

/* Lepas storage MSC + wear levelling supaya fs_healing bisa memformat
 * partisi (factory reset). Setelah ini MSC tidak aktif sampai reboot. */
void usb_composite_release_msc(void);
