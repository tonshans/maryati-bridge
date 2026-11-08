#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_state";

#define MB_NVS_NAMESPACE     "mb_state"
#define MB_NVS_KEY_CFG       "cfg"
#define MB_NVS_CFG_VERSION   1

/* Base58 tanpa karakter ambigu: 0, O, 1, l, I */
/* Literal dari AI_Rules.md/PRD.md apa adanya. Dokumen menyebut "58 karakter",
 * tetapi literal 57 karakter unik (0,O,1,l,I memang dibuang). */
static const char MB_BASE58[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
#define MB_BASE58_LEN ((int)(sizeof(MB_BASE58) - 1))

_Static_assert(sizeof(MB_BASE58) - 1 == 57, "Base58 alphabet length changed");

#define MB_RANDOM_SSID_CHARS 4
#define MB_RANDOM_PASS_CHARS 8
#define MB_DEFAULT_HOST_IP   "192.168.33.2"

typedef struct {
    uint16_t version;
    sys_config_t cfg;
} mb_nvs_cfg_blob_t;

static SemaphoreHandle_t s_lock;
static app_state_t s_state;
static sys_config_t s_config;

static uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void config_set_defaults(sys_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->default_boot_mode = MODE_P2P_PROXY;
    cfg->enable_msc = true;
    cfg->hidden_ssid = false;
    cfg->tx_power_dbm = 20;
    cfg->usb_vid = 0x0BDA;
    cfg->usb_pid = 0x8152;
    /* MAC unicast contoh dari PRD; bisa diganti lewat config.txt / Web GUI. */
    static const uint8_t default_mac[6] = { 0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02 };
    memcpy(cfg->custom_mac, default_mac, sizeof(default_mac));
}

static void config_sanitize(sys_config_t *cfg)
{
    if ((unsigned)cfg->default_boot_mode > MODE_ENTERPRISE_STEALTH) {
        cfg->default_boot_mode = MODE_P2P_PROXY;
    }
    if (cfg->tx_power_dbm < 8) {
        cfg->tx_power_dbm = 8;
    } else if (cfg->tx_power_dbm > 20) {
        cfg->tx_power_dbm = 20;
    }
    cfg->static_ssid[MB_SSID_MAX_LEN - 1] = '\0';
    cfg->static_pass[MB_PASS_MAX_LEN - 1] = '\0';

    bool mac_zero = true;
    bool mac_multicast = (cfg->custom_mac[0] & 0x01) != 0;
    for (int i = 0; i < 6; i++) {
        if (cfg->custom_mac[i] != 0) {
            mac_zero = false;
        }
    }
    if (mac_zero || mac_multicast) {
        static const uint8_t fallback[6] = { 0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02 };
        memcpy(cfg->custom_mac, fallback, sizeof(fallback));
    }

    if (cfg->usb_vid == 0) {
        cfg->usb_vid = 0x0BDA;
    }
    if (cfg->usb_pid == 0) {
        cfg->usb_pid = 0x8152;
    }
}

static esp_err_t config_load_from_nvs(sys_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MB_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return err;
    } else if (err != ESP_OK) {
        return err;
    }

    mb_nvs_cfg_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, MB_NVS_KEY_CFG, &blob, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(blob) && blob.version == MB_NVS_CFG_VERSION) {
        *out = blob.cfg;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_VERSION;
    }
    return err;
}

static esp_err_t config_save_to_nvs(const sys_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    mb_nvs_cfg_blob_t blob = { .version = MB_NVS_CFG_VERSION, .cfg = *cfg };
    err = nvs_set_blob(h, MB_NVS_KEY_CFG, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void generate_base58(char *out, size_t out_len, size_t chars)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t n = chars;
    if (n > out_len - 1) {
        n = out_len - 1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = MB_BASE58[esp_random() % MB_BASE58_LEN];
    }
    out[n] = '\0';
}

esp_err_t app_state_init(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    config_set_defaults(&s_config);
    esp_err_t err = config_load_from_nvs(&s_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config loaded from NVS");
    } else {
        ESP_LOGW(TAG, "NVS config not loaded (%s), using defaults", esp_err_to_name(err));
        config_set_defaults(&s_config);
    }
    config_sanitize(&s_config);

    memset(&s_state, 0, sizeof(s_state));
    s_state.current_mode = s_config.default_boot_mode;
    s_state.preview_menu_mode = s_config.default_boot_mode;
    s_state.oled_state = OLED_STATE_ACTIVE;
    s_state.last_input_timestamp_sec = now_sec();
    strlcpy(s_state.host_ip, MB_DEFAULT_HOST_IP, sizeof(s_state.host_ip));

    if (s_config.use_static_cred && strlen(s_config.static_pass) >= 8 && s_config.static_ssid[0] != '\0') {
        strlcpy(s_state.active_ssid, s_config.static_ssid, sizeof(s_state.active_ssid));
        strlcpy(s_state.active_password, s_config.static_pass, sizeof(s_state.active_password));
    } else {
        app_state_generate_random_credentials(s_state.active_ssid, sizeof(s_state.active_ssid),
                                              s_state.active_password, sizeof(s_state.active_password));
    }

    ESP_LOGI(TAG, "mode=%d ssid=%s", s_state.current_mode, s_state.active_ssid);
    return ESP_OK;
}

void app_state_get(app_state_t *out_state)
{
    if (out_state == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_state = s_state;
    xSemaphoreGive(s_lock);
}

void app_state_get_config(sys_config_t *out_cfg)
{
    if (out_cfg == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_cfg = s_config;
    xSemaphoreGive(s_lock);
}

esp_err_t app_state_set_mode(app_mode_t new_mode)
{
    if ((unsigned)new_mode > MODE_ENTERPRISE_STEALTH) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.current_mode = new_mode;
    s_state.preview_menu_mode = new_mode;
    s_state.menu_selection_active = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_update_config(const sys_config_t *new_cfg, bool save_to_nvs)
{
    if (new_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sys_config_t cfg = *new_cfg;
    config_sanitize(&cfg);

    if (save_to_nvs) {
        esp_err_t err = config_save_to_nvs(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void app_state_generate_random_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
{
    if (out_ssid == NULL || out_pass == NULL) {
        return;
    }
    if (ssid_len < 3 + MB_RANDOM_SSID_CHARS + 1 || pass_len < MB_RANDOM_PASS_CHARS + 1) {
        ESP_LOGE(TAG, "credential buffer too small");
        if (ssid_len > 0) {
            out_ssid[0] = '\0';
        }
        if (pass_len > 0) {
            out_pass[0] = '\0';
        }
        return;
    }

    strlcpy(out_ssid, "MB-", ssid_len);
    generate_base58(out_ssid + 3, ssid_len - 3, MB_RANDOM_SSID_CHARS);
    generate_base58(out_pass, pass_len, MB_RANDOM_PASS_CHARS);
}

void app_state_record_button_activity(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.last_input_timestamp_sec = now_sec();
    s_state.oled_state = OLED_STATE_ACTIVE;
    xSemaphoreGive(s_lock);
}

void app_state_set_host_info(const char *ip_str, bool proxy_on)
{
    if (ip_str == NULL || ip_str[0] == '\0') {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_state.host_ip, ip_str, sizeof(s_state.host_ip));
    s_state.proxy_active_on_host = proxy_on;
    xSemaphoreGive(s_lock);
}

void app_state_inc_counters(uint32_t usb_rx, uint32_t usb_tx, uint32_t wifi_rx, uint32_t wifi_tx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.usb_rx_frames += usb_rx;
    s_state.usb_tx_frames += usb_tx;
    s_state.wifi_rx_frames += wifi_rx;
    s_state.wifi_tx_frames += wifi_tx;
    xSemaphoreGive(s_lock);
}

void app_state_set_connected_clients(uint8_t count)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.connected_clients = count;
    xSemaphoreGive(s_lock);
}

void app_state_set_oled_state(oled_power_state_t state)
{
    if ((unsigned)state > OLED_STATE_OFF) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.oled_state = state;
    xSemaphoreGive(s_lock);
}

void app_state_set_preview_menu(app_mode_t preview, bool selection_active)
{
    if ((unsigned)preview > MODE_ENTERPRISE_STEALTH) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.preview_menu_mode = preview;
    s_state.menu_selection_active = selection_active;
    xSemaphoreGive(s_lock);
}

esp_err_t app_state_set_credentials(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len >= MB_SSID_MAX_LEN || pass_len < 8 || pass_len >= MB_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_state.active_ssid, ssid, sizeof(s_state.active_ssid));
    strlcpy(s_state.active_password, pass, sizeof(s_state.active_password));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
