#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_state.h"
#include "button_input.h"
#include "fs_healing.h"
#include "mac_nat.h"
#include "ui_oled.h"
#include "usb_composite.h"
#include "web_gui.h"
#include "wifi_bridge.h"

static const char *TAG = "main";

/* NVS bisa korup setelah update partisi atau factory reset yang terputus. */
static esp_err_t nvs_init_or_erase(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS perlu dihapus (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init_or_erase());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(app_state_init());

    /* Filesystem hanya membawa config.txt; bridge tetap bisa jalan tanpa FAT. */
    err = fs_healing_init_and_load_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fs_healing init gagal (%s), lanjut dengan config NVS", esp_err_to_name(err));
    }

    err = mac_nat_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mac_nat init gagal (%s)", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(wifi_bridge_init());

    err = usb_composite_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB composite init gagal (%s)", esp_err_to_name(err));
    }

    err = web_gui_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web GUI gagal start (%s)", esp_err_to_name(err));
    }

    err = ui_oled_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED tidak tersedia (%s)", esp_err_to_name(err));
    }

    err = button_input_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Button input gagal start (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Maryati Bridge siap");
}
