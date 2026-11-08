#include "wifi_bridge.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_private/wifi.h"

#include "board_pins.h"
#include "mac_nat.h"
#include "usb_composite.h"

static const char *TAG = "wifi_bridge";

#define MB_ETH_HDR_LEN          14
#define MB_MAX_FRAME_LEN        1514
#define MB_ETH_TYPE_IPV4        0x0800
#define MB_CTRL_UDP_PORT        33333
#define MB_CTRL_PAYLOAD_MAX     64
#define MB_AP_CHANNEL           6
#define MB_AP_IP0               192
#define MB_AP_IP1               168
#define MB_AP_IP2               33
#define MB_AP_IP3               1
#define MB_STEALTH_TX_POWER     34  /* 0.25 dBm unit -> ~8.5 dBm */

static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_ap_start_sem;
static uint8_t s_ap_mac[6];
static uint8_t s_spoof_mac[6];
static volatile app_mode_t s_mode;
static volatile bool s_ap_started;
static volatile bool s_initialized;

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool is_broadcast(const uint8_t mac[6])
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return memcmp(mac, bcast, 6) == 0;
}

static void refresh_client_count(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
        app_state_set_connected_clients((uint8_t)list.num);
    }
}

/* RX L2 dari klien Wi-Fi: teruskan ke USB, dan salin ke LwIP bila frame
 * ditujukan ke ESP (broadcast / MAC AP) untuk DHCP + Web GUI. */
static esp_err_t wifi_ap_rx_cb(void *buffer, uint16_t len, void *eb)
{
    if (!s_ap_started || buffer == NULL || len < MB_ETH_HDR_LEN ||
        len > MB_MAX_FRAME_LEN) {
        esp_wifi_internal_free_rx_buffer(eb ? eb : buffer);
        return ESP_FAIL;
    }

    uint8_t *frame = (uint8_t *)buffer;
    void *free_arg = eb ? eb : buffer;
    app_state_inc_counters(0, 0, 1, 0);

    bool bcast = is_broadcast(frame);
    bool to_ap = memcmp(frame, s_ap_mac, 6) == 0;

    if (s_mode == MODE_ENTERPRISE_STEALTH) {
        mac_nat_translate_tx(frame, len, s_spoof_mac);
    }

    if (!to_ap) {
        if (usb_composite_send_eth_frame(frame, len) == ESP_OK) {
            app_state_inc_counters(0, 1, 0, 0);
        }
    }

    if (bcast || to_ap) {
        /* esp_netif_receive memiliki buffer dan membebaskannya via wifi_free. */
        return esp_netif_receive(s_ap_netif, frame, len, free_arg);
    }

    esp_wifi_internal_free_rx_buffer(free_arg);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (id) {
    case WIFI_EVENT_AP_START:
        s_ap_started = true;
        if (esp_wifi_get_mac(WIFI_IF_AP, s_ap_mac) != ESP_OK) {
            ESP_LOGW(TAG, "gagal membaca MAC SoftAP");
        }
        /* Timpa rxcb bawaan esp_netif: kita yang memilah frame LwIP vs USB. */
        if (esp_wifi_internal_reg_rxcb(WIFI_IF_AP, wifi_ap_rx_cb) != ESP_OK) {
            ESP_LOGE(TAG, "gagal mendaftarkan rxcb AP");
        }
        if (xSemaphoreGive(s_ap_start_sem) != pdTRUE) {
            /* sudah diberikan, tidak masalah */
        }
        break;
    case WIFI_EVENT_AP_STOP:
        s_ap_started = false;
        app_state_set_connected_clients(0);
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        refresh_client_count();
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        refresh_client_count();
        break;
    default:
        break;
    }
}

static esp_err_t build_ap_config(const char *ssid, const char *pass, app_mode_t mode)
{
    sys_config_t cfg;
    app_state_get_config(&cfg);

    wifi_config_t wc = { 0 };
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(wc.ap.ssid)) {
        ssid_len = sizeof(wc.ap.ssid);
    }
    memcpy(wc.ap.ssid, ssid, ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    strlcpy((char *)wc.ap.password, pass, sizeof(wc.ap.password));

    wc.ap.channel = MB_AP_CHANNEL;
    wc.ap.max_connection = MB_MAX_WIFI_CLIENTS;
    wc.ap.beacon_interval = 100;

    if (mode == MODE_ENTERPRISE_STEALTH) {
        wc.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        wc.ap.pmf_cfg.capable = true;
        wc.ap.pmf_cfg.required = true;
        wc.ap.ssid_hidden = 1;
    } else {
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wc.ap.pmf_cfg.capable = false;
        wc.ap.pmf_cfg.required = false;
        wc.ap.ssid_hidden = cfg.hidden_ssid ? 1 : 0;
    }

    return esp_wifi_set_config(WIFI_IF_AP, &wc);
}

static esp_err_t configure_dhcp(app_mode_t mode)
{
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }

    /* IP manajemen tetap 192.168.33.1 di semua mode agar Web GUI dapat
     * diakses lewat IP statis, tanpa default gateway. */
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ESP_IP4TOADDR(MB_AP_IP0, MB_AP_IP1, MB_AP_IP2, MB_AP_IP3);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ip.gw.addr = 0;
    err = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (err != ESP_OK) {
        return err;
    }

    if (mode != MODE_P2P_PROXY) {
        /* Mode 1/2: semua DHCP diteruskan ke LAN asli lewat jembatan L2. */
        return ESP_OK;
    }

    /* Mode 0 tidak boleh mengirim Option 3 (router) maupun DNS. */
    uint8_t offer = 0;
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                               &offer, sizeof(offer)),
        TAG, "set offer_router");
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer, sizeof(offer)),
        TAG, "set offer_dns");

    return esp_netif_dhcps_start(s_ap_netif);
}

static esp_err_t apply_mode_runtime(app_mode_t mode)
{
    sys_config_t cfg;
    app_state_get_config(&cfg);
    app_state_t st;
    app_state_get(&st);

    esp_err_t err = build_ap_config(st.active_ssid, st.active_password, mode);
    if (err != ESP_OK) {
        return err;
    }

    int8_t power = (mode == MODE_ENTERPRISE_STEALTH) ? MB_STEALTH_TX_POWER
                                                     : (int8_t)(cfg.tx_power_dbm * 4);
    err = esp_wifi_set_max_tx_power(power);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set tx power: %s", esp_err_to_name(err));
    }

    err = configure_dhcp(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "konfigurasi DHCP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mode %d aktif (tx=%d, hidden=%d)",
             mode, power, (mode == MODE_ENTERPRISE_STEALTH) || cfg.hidden_ssid);
    return ESP_OK;
}

esp_err_t wifi_bridge_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    app_state_t st;
    app_state_get(&st);
    sys_config_t cfg;
    app_state_get_config(&cfg);

    s_mode = st.current_mode;
    memcpy(s_spoof_mac, cfg.custom_mac, sizeof(s_spoof_mac));

    s_ap_start_sem = xSemaphoreCreateBinary();
    if (s_ap_start_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }

    err = build_ap_config(st.active_ssid, st.active_password, s_mode);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_ap_start_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "AP start event tidak diterima, lanjut best-effort");
    }

    /* Pastikan handler kita menang atas handler bawaan esp_netif. */
    err = esp_wifi_internal_reg_rxcb(WIFI_IF_AP, wifi_ap_rx_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reg rxcb AP gagal: %s", esp_err_to_name(err));
    }

    s_initialized = true;

    err = apply_mode_runtime(s_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply mode awal gagal: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t wifi_bridge_apply_mode(app_mode_t mode)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((unsigned)mode > MODE_ENTERPRISE_STEALTH) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = app_state_set_mode(mode);
    if (err != ESP_OK) {
        return err;
    }

    mac_nat_clear();
    s_mode = mode;

    if (!s_ap_started) {
        return ESP_OK;
    }
    return apply_mode_runtime(mode);
}

esp_err_t wifi_bridge_apply_credentials(const char *ssid, const char *pass)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len >= MB_SSID_MAX_LEN ||
        pass_len < 8 || pass_len >= MB_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = build_ap_config(ssid, pass, s_mode);
    if (err != ESP_OK) {
        return err;
    }

    /* Terapkan ulang DHCP/hidden/tx power untuk mode yang sedang aktif. */
    return apply_mode_runtime(s_mode);
}

esp_err_t wifi_bridge_from_usb(const uint8_t *frame, uint16_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame == NULL || len < MB_ETH_HDR_LEN || len > MB_MAX_FRAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    app_state_inc_counters(1, 0, 0, 0);

    if (read_be16(frame + 12) == MB_ETH_TYPE_IPV4 &&
        len >= MB_ETH_HDR_LEN + 20) {
        /* Deteksi paket kontrol UDP 33333 dari script PC Host. */
        const uint8_t *ip = frame + MB_ETH_HDR_LEN;
        size_t ihl = (size_t)(ip[0] & 0x0f) * 4U;
        if ((ip[0] >> 4) == 4 && ihl >= 20 &&
            len >= MB_ETH_HDR_LEN + ihl + 8 && ip[9] == 17 /* UDP */ &&
            (read_be16(ip + 6) & 0x1fff) == 0) {
            const uint8_t *udp = ip + ihl;
            uint16_t dport = read_be16(udp + 2);
            uint16_t ulen = read_be16(udp + 4);
            if (dport == MB_CTRL_UDP_PORT && ulen >= 8 &&
                MB_ETH_HDR_LEN + ihl + ulen <= len) {
                uint16_t paylen = (uint16_t)(ulen - 8);
                char payload[MB_CTRL_PAYLOAD_MAX];
                if (paylen > sizeof(payload) - 1) {
                    paylen = sizeof(payload) - 1;
                }
                memcpy(payload, udp + 8, paylen);
                payload[paylen] = '\0';
                if (strncmp(payload, "CMD:", 4) == 0) {
                    int new_mode = -1;
                    char ip_str[MB_IP_STR_MAX_LEN];
                    if (sscanf(payload, "CMD:MODE=%d:%15s", &new_mode, ip_str) == 2) {
                        esp_ip4_addr_t parsed;
                        if (new_mode >= 0 && new_mode <= 2 &&
                            esp_netif_str_to_ip4(ip_str, &parsed) == ESP_OK) {
                            ESP_LOGW(TAG, "perintah host: mode %d, host %s", new_mode, ip_str);
                            wifi_bridge_apply_mode((app_mode_t)new_mode);
                            app_state_set_host_info(ip_str, false);
                        }
                    } else if (sscanf(payload, "CMD:PROXY_ON:%15s", ip_str) == 1) {
                        esp_ip4_addr_t parsed;
                        if (esp_netif_str_to_ip4(ip_str, &parsed) == ESP_OK) {
                            ESP_LOGW(TAG, "proxy host aktif di %s", ip_str);
                            app_state_set_host_info(ip_str, true);
                        }
                    }
                    return ESP_OK; /* paket kontrol tidak diteruskan */
                }
            }
        }
    }

    if (s_mode == MODE_ENTERPRISE_STEALTH) {
        mac_nat_translate_rx((uint8_t *)frame, len);
    }

    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_AP, (void *)frame, len);
    if (err == ESP_OK) {
        app_state_inc_counters(0, 0, 0, 1);
    } else {
        ESP_LOGD(TAG, "TX Wi-Fi gagal: %s", esp_err_to_name(err));
    }

    /* Salinan lokal untuk DHCP/Web GUI ESP32 (broadcast atau MAC AP). */
    bool to_ap = memcmp(frame, s_ap_mac, 6) == 0;
    if (is_broadcast(frame) || to_ap) {
        uint8_t *copy = wifi_malloc(len);
        if (copy != NULL) {
            memcpy(copy, frame, len);
            /* esp_netif_receive memiliki buffer dan membebaskannya via wifi_free. */
            if (esp_netif_receive(s_ap_netif, copy, len, copy) != ESP_OK) {
                ESP_LOGD(TAG, "inject LwIP gagal");
            }
        }
    }

    return err;
}
