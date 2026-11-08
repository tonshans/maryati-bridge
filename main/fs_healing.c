#include "fs_healing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "app_state.h"

static const char *TAG = "fs_healing";

#define MB_FS_BASE_PATH   "/fatfs"
#define MB_FS_PARTITION   "storage"

/* Cluster size 512 B: pada partisi 2.4 MB (S2) menghasilkan ~4800 klaster,
 * di atas batas 4085 klaster sehingga FatFS memilih FAT16, bukan FAT12. */
#define MB_FS_ALLOC_UNIT  512

_Static_assert(CONFIG_WL_SECTOR_SIZE == MB_FS_ALLOC_UNIT,
               "WL sector must be 512 B for the forced FAT16 layout");

/* Simbol dari EMBED_FILES di components/rodata_assets (Langkah 4). */
extern const uint8_t _binary_config_txt_start[];
extern const uint8_t _binary_config_txt_end[];
extern const uint8_t _binary_Start_Bridge_bat_start[];
extern const uint8_t _binary_Start_Bridge_bat_end[];
extern const uint8_t _binary_Start_Stealth_bat_start[];
extern const uint8_t _binary_Start_Stealth_bat_end[];
extern const uint8_t _binary_Start_Proxy_bat_start[];
extern const uint8_t _binary_Start_Proxy_bat_end[];
extern const uint8_t _binary_start_bridge_sh_start[];
extern const uint8_t _binary_start_bridge_sh_end[];
extern const uint8_t _binary_start_stealth_sh_start[];
extern const uint8_t _binary_start_stealth_sh_end[];
extern const uint8_t _binary_start_proxy_sh_start[];
extern const uint8_t _binary_start_proxy_sh_end[];

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} mb_embedded_file_t;

static const mb_embedded_file_t s_files[] = {
    { "config.txt",       _binary_config_txt_start,       _binary_config_txt_end },
    { "Start_Bridge.bat", _binary_Start_Bridge_bat_start, _binary_Start_Bridge_bat_end },
    { "Start_Stealth.bat", _binary_Start_Stealth_bat_start, _binary_Start_Stealth_bat_end },
    { "Start_Proxy.bat",  _binary_Start_Proxy_bat_start,  _binary_Start_Proxy_bat_end },
    { "start_bridge.sh",  _binary_start_bridge_sh_start,  _binary_start_bridge_sh_end },
    { "start_stealth.sh", _binary_start_stealth_sh_start, _binary_start_stealth_sh_end },
    { "start_proxy.sh",   _binary_start_proxy_sh_start,   _binary_start_proxy_sh_end },
};

#define MB_FILE_COUNT (sizeof(s_files) / sizeof(s_files[0]))

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_vfs_mounted;

static const esp_vfs_fat_mount_config_t s_mount_cfg = {
    .format_if_mount_failed = true,
    .max_files = 4,
    .allocation_unit_size = MB_FS_ALLOC_UNIT,
    .disk_status_check_enable = false,
    .use_one_fat = false,
};

static esp_err_t mount_vfs(void)
{
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MB_FS_BASE_PATH, MB_FS_PARTITION,
                                                     &s_mount_cfg, &s_wl_handle);
    if (err == ESP_OK) {
        s_vfs_mounted = true;
    }
    return err;
}

static esp_err_t unmount_vfs(void)
{
    if (!s_vfs_mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(MB_FS_BASE_PATH, s_wl_handle);
    if (err == ESP_OK) {
        s_vfs_mounted = false;
    }
    return err;
}

static esp_err_t write_embedded_file(const mb_embedded_file_t *file)
{
    char path[64];
    snprintf(path, sizeof(path), MB_FS_BASE_PATH "/%s", file->name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for write", path);
        return ESP_FAIL;
    }

    size_t len = (size_t)(file->end - file->start);
    size_t written = fwrite(file->start, 1, len, f);
    int close_err = fclose(f);
    if (written != len || close_err != 0) {
        ESP_LOGE(TAG, "short write on %s (%u/%u)", path, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "restored %s (%u bytes)", file->name, (unsigned)len);
    return ESP_OK;
}

static bool file_is_healthy(const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), MB_FS_BASE_PATH "/%s", name);

    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

static esp_err_t restore_missing_files(void)
{
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < MB_FILE_COUNT; i++) {
        if (file_is_healthy(s_files[i].name)) {
            continue;
        }
        esp_err_t err = write_embedded_file(&s_files[i]);
        if (err != ESP_OK) {
            result = err;
        }
    }
    return result;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static void apply_kv(sys_config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "use_static") == 0) {
        cfg->use_static_cred = atoi(value) != 0;
    } else if (strcmp(key, "static_ssid") == 0) {
        strlcpy(cfg->static_ssid, value, sizeof(cfg->static_ssid));
    } else if (strcmp(key, "static_pass") == 0) {
        strlcpy(cfg->static_pass, value, sizeof(cfg->static_pass));
    } else if (strcmp(key, "default_mode") == 0) {
        int mode = atoi(value);
        if (mode >= MODE_P2P_PROXY && mode <= MODE_ENTERPRISE_STEALTH) {
            cfg->default_boot_mode = (app_mode_t)mode;
        }
    } else if (strcmp(key, "enable_msc") == 0) {
        cfg->enable_msc = atoi(value) != 0;
    } else if (strcmp(key, "hidden_ssid") == 0) {
        cfg->hidden_ssid = atoi(value) != 0;
    } else if (strcmp(key, "tx_power_dbm") == 0) {
        cfg->tx_power_dbm = (int8_t)atoi(value);
    } else if (strcmp(key, "custom_mac") == 0) {
        unsigned int m[6];
        if (sscanf(value, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; i++) {
                cfg->custom_mac[i] = (uint8_t)m[i];
            }
        } else {
            ESP_LOGW(TAG, "invalid custom_mac '%s'", value);
        }
    } else if (strcmp(key, "usb_vid") == 0) {
        cfg->usb_vid = (uint16_t)strtoul(value, NULL, 0);
    } else if (strcmp(key, "usb_pid") == 0) {
        cfg->usb_pid = (uint16_t)strtoul(value, NULL, 0);
    } else {
        ESP_LOGD(TAG, "unknown config key '%s'", key);
    }
}

static void load_config_txt(void)
{
    FILE *f = fopen(MB_FS_BASE_PATH "/config.txt", "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "config.txt not readable, keeping NVS/default config");
        return;
    }

    sys_config_t cfg;
    app_state_get_config(&cfg);

    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }
        char *eq = strchr(text, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        apply_kv(&cfg, trim(text), trim(eq + 1));
    }
    fclose(f);

    esp_err_t err = app_state_update_config(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to apply config.txt: %s", esp_err_to_name(err));
    }
}

esp_err_t fs_healing_init_and_load_config(void)
{
    if (s_vfs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = mount_vfs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAT mount/format failed: %s", esp_err_to_name(err));
        return err;
    }

    err = restore_missing_files();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "some embedded files could not be restored");
    }

    load_config_txt();

    /* Single-writer rule: lepas VFS + wear levelling sebelum MSC membuka
     * blok storage (MSC memasang storage-nya sendiri di usb_composite.c). */
    esp_err_t unmount_err = unmount_vfs();
    if (unmount_err != ESP_OK) {
        ESP_LOGE(TAG, "VFS unmount failed: %s", esp_err_to_name(unmount_err));
        return unmount_err;
    }

    return err;
}

esp_err_t fs_healing_write_file(const char *name, const void *data, size_t len)
{
    if (name == NULL || name[0] == '\0' || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool was_mounted = s_vfs_mounted;
    if (!was_mounted) {
        esp_err_t err = mount_vfs();
        if (err != ESP_OK) {
            return err;
        }
    }

    char path[96];
    snprintf(path, sizeof(path), MB_FS_BASE_PATH "/%s", name);

    esp_err_t err = ESP_OK;
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        err = ESP_FAIL;
    } else {
        size_t written = fwrite(data, 1, len, f);
        if (written != len) {
            err = ESP_FAIL;
        }
        if (fclose(f) != 0) {
            err = ESP_FAIL;
        }
    }

    if (!was_mounted) {
        esp_err_t unmount_err = unmount_vfs();
        if (err == ESP_OK) {
            err = unmount_err;
        }
    }
    return err;
}

esp_err_t fs_healing_factory_reset(void)
{
    if (s_vfs_mounted) {
        ESP_RETURN_ON_ERROR(unmount_vfs(), TAG, "unmount before format failed");
    }

    /* shortcut: factory reset saat MSC aktif bisa bertabrakan dengan host;
     * reboot setelah reset. */
    esp_vfs_fat_mount_config_t cfg = s_mount_cfg;
    ESP_RETURN_ON_ERROR(esp_vfs_fat_spiflash_format_cfg_rw_wl(MB_FS_BASE_PATH, MB_FS_PARTITION, &cfg),
                        TAG, "format failed");

    ESP_RETURN_ON_ERROR(mount_vfs(), TAG, "remount after format failed");
    ESP_RETURN_ON_ERROR(restore_missing_files(), TAG, "restore after format failed");
    load_config_txt();
    ESP_RETURN_ON_ERROR(unmount_vfs(), TAG, "unmount after format failed");

    ESP_LOGI(TAG, "factory reset done");
    return ESP_OK;
}
