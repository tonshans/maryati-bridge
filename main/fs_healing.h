#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Mount /fatfs, restore missing files from .rodata, load config.txt, then
 * release VFS + wear levelling so TinyUSB MSC owns the storage exclusively. */
esp_err_t fs_healing_init_and_load_config(void);

/* Reformat the FAT partition and restore all default files from .rodata.
 * Reboot afterwards: the host may still have the MSC drive open. */
esp_err_t fs_healing_factory_reset(void);

/* Tulis satu file di root FAT (mount sementara bila perlu).
 * Hanya dipakai saat MSC tidak memegang storage (enable_msc=false). */
esp_err_t fs_healing_write_file(const char *name, const void *data, size_t len);
