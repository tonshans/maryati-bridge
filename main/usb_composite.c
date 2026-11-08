#include "usb_composite.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "wear_levelling.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "tinyusb_msc.h"

#include "app_state.h"
#include "board_pins.h"
#include "wifi_bridge.h"

static const char *TAG = "usb_composite";

#define MB_MSC_BASE_PATH        "/usb"
#define MB_NET_MTU              1514
#define MB_USB_TX_TIMEOUT_TICKS pdMS_TO_TICKS(20)

/* Indeks string descriptor. Urutan ini mengikuti enum STRID di esp_tinyusb
 * untuk kombinasi MSC + NCM (CDC/MTP/HID nonaktif). */
enum {
    MB_STRID_LANGID = 0,
    MB_STRID_MANUFACTURER,
    MB_STRID_PRODUCT,
    MB_STRID_SERIAL,
    MB_STRID_MSC,
    MB_STRID_NET,
    MB_STRID_MAC,
    MB_STRID_COUNT
};

/* Endpoint CDC-NCM + MSC (FS cukup: 1 notif IN, 1 data IN/OUT per fungsi). */
#define MB_EP_NCM_NOTIF         0x81
#define MB_EP_NCM_OUT           0x02
#define MB_EP_NCM_IN            0x82
#define MB_EP_MSC_OUT           0x03
#define MB_EP_MSC_IN            0x83

static const uint8_t s_cfg_ncm[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0,
                          TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN,
                          0, 100),
    TUD_CDC_NCM_DESCRIPTOR(0, MB_STRID_NET, MB_STRID_MAC,
                           MB_EP_NCM_NOTIF, 64,
                           MB_EP_NCM_OUT, MB_EP_NCM_IN, 64,
                           MB_NET_MTU),
};

static const uint8_t s_cfg_ncm_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0,
                          TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_MSC_DESC_LEN,
                          0, 250),
    TUD_CDC_NCM_DESCRIPTOR(0, MB_STRID_NET, MB_STRID_MAC,
                           MB_EP_NCM_NOTIF, 64,
                           MB_EP_NCM_OUT, MB_EP_NCM_IN, 64,
                           MB_NET_MTU),
    TUD_MSC_DESCRIPTOR(2, MB_STRID_MSC, MB_EP_MSC_OUT, MB_EP_MSC_IN, 64),
};

static const char s_langid[] = { 0x09, 0x04 };
static char s_serial[13];
static const char *s_strings[MB_STRID_COUNT] = {
    s_langid,
    "Realtek",
    "USB 10/100 LAN",
    s_serial,
    "Mass Storage",
    "Network",
    "", /* MB_STRID_MAC: diisi esp_tinyusb dengan MAC NCM saat init. */
};

static tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0,
    .idProduct = 0,
    .bcdDevice = 0x0100,
    .iManufacturer = MB_STRID_MANUFACTURER,
    .iProduct = MB_STRID_PRODUCT,
    .iSerialNumber = MB_STRID_SERIAL,
    .bNumConfigurations = 1,
};

static SemaphoreHandle_t s_tx_lock;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc;
static bool s_initialized;

static esp_err_t usb_net_rx(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    return wifi_bridge_from_usb((const uint8_t *)buffer, len);
}

static esp_err_t msc_start(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = wl_mount(part, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount gagal: %s", esp_err_to_name(err));
        s_wl_handle = WL_INVALID_HANDLE;
        return err;
    }

    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium = { .wl_handle = s_wl_handle },
        .fat_fs = {
            .base_path = MB_MSC_BASE_PATH,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 4,
                .allocation_unit_size = 512,
                .disk_status_check_enable = false,
                .use_one_fat = false,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };

    err = tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_msc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage gagal: %s", esp_err_to_name(err));
        s_msc = NULL;
    }
    return err;
}

esp_err_t usb_composite_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    sys_config_t cfg;
    app_state_get_config(&cfg);

    uint8_t base_mac[6] = { 0 };
    esp_err_t err = esp_read_mac(base_mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             base_mac[0], base_mac[1], base_mac[2],
             base_mac[3], base_mac[4], base_mac[5]);

    bool use_msc = cfg.enable_msc;
    if (use_msc) {
        if (msc_start() != ESP_OK) {
            ESP_LOGW(TAG, "MSC dinonaktifkan karena gagal init");
            use_msc = false;
        }
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_device_desc.idVendor = cfg.usb_vid;
    s_device_desc.idProduct = cfg.usb_pid;

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task.xCoreID = MB_TASK_CORE_USB;
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.string = s_strings;
    tusb_cfg.descriptor.string_count = MB_STRID_COUNT;
    tusb_cfg.descriptor.full_speed_config = use_msc ? s_cfg_ncm_msc : s_cfg_ncm;

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install gagal: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t usb_mac[6] = {
        0x02, 0x4D, 0x42, base_mac[3], base_mac[4], base_mac[5]
    };
    const tinyusb_net_config_t net_cfg = {
        .mac_addr = { usb_mac[0], usb_mac[1], usb_mac[2],
                      usb_mac[3], usb_mac[4], usb_mac[5] },
        .on_recv_callback = usb_net_rx,
        .free_tx_buffer = NULL,
        .on_init_callback = NULL,
        .user_context = NULL,
    };
    err = tinyusb_net_init(&net_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init gagal: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB composite siap (VID=%04X PID=%04X, MSC=%s)",
             cfg.usb_vid, cfg.usb_pid, use_msc ? "on" : "off");
    return ESP_OK;
}

esp_err_t usb_composite_send_eth_frame(const uint8_t *frame, uint16_t len)
{
    if (!s_initialized || s_tx_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame == NULL || len < 14 || len > MB_NET_MTU) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t err = tinyusb_net_send_sync((void *)frame, len, NULL, MB_USB_TX_TIMEOUT_TICKS);
    xSemaphoreGive(s_tx_lock);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "TX USB gagal: %s", esp_err_to_name(err));
    }
    return err;
}

bool usb_composite_msc_active(void)
{
    return s_msc != NULL;
}

esp_err_t usb_composite_msc_write_file(const char *name, const void *data, size_t len)
{
    if (name == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_msc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(s_msc, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount APP gagal: %s", esp_err_to_name(err));
        return err;
    }

    char path[64];
    snprintf(path, sizeof(path), MB_MSC_BASE_PATH "/%s", name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "tidak bisa membuka %s", path);
        tinyusb_msc_set_storage_mount_point(s_msc, TINYUSB_MSC_STORAGE_MOUNT_USB);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    int close_err = fclose(f);
    err = (written == len && close_err == 0) ? ESP_OK : ESP_FAIL;

    esp_err_t remount_err = tinyusb_msc_set_storage_mount_point(s_msc, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (remount_err != ESP_OK) {
        ESP_LOGW(TAG, "remount USB gagal: %s", esp_err_to_name(remount_err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tulis %s gagal (%u/%u)", path, (unsigned)written, (unsigned)len);
    }
    return err;
}

void usb_composite_release_msc(void)
{
    if (s_msc != NULL) {
        esp_err_t err = tinyusb_msc_delete_storage(s_msc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete MSC storage: %s", esp_err_to_name(err));
        }
        s_msc = NULL;
    }
    s_wl_handle = WL_INVALID_HANDLE;
}
