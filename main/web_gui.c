#include "web_gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"

#include "app_state.h"
#include "fs_healing.h"
#include "usb_composite.h"
#include "wifi_bridge.h"

static const char *TAG = "web_gui";

#define MB_WEB_BODY_MAX      1024
#define MB_WEB_AUTH_MAX      192
#define MB_WEB_CONFIG_MAX    512

extern const uint8_t _binary_index_html_start[];
extern const uint8_t _binary_index_html_end[];

static httpd_handle_t s_server;

static const char *mode_name(app_mode_t mode)
{
    switch (mode) {
    case MODE_TRANSPARENT:
        return "TRANSPARENT";
    case MODE_ENTERPRISE_STEALTH:
        return "ENTERPRISE_STEALTH";
    case MODE_P2P_PROXY:
    default:
        return "P2P_PROXY";
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "error", message);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static bool request_authorized(httpd_req_t *req)
{
    app_state_t st;
    app_state_get(&st);

    char header[MB_WEB_AUTH_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }

    if (strncmp(header, "Bearer ", 7) == 0) {
        return strcmp(header + 7, st.active_password) == 0;
    }

    if (strncmp(header, "Basic ", 6) == 0) {
        unsigned char decoded[160];
        size_t out_len = 0;
        size_t in_len = strlen(header + 6);
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                                  (const unsigned char *)header + 6, in_len) != 0) {
            return false;
        }
        decoded[out_len] = '\0';
        const char *colon = strchr((const char *)decoded, ':');
        if (colon == NULL) {
            return false;
        }
        return strcmp(colon + 1, st.active_password) == 0;
    }

    return false;
}

static bool require_auth(httpd_req_t *req)
{
    if (request_authorized(req)) {
        return true;
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Maryati Bridge\"");
    send_error(req, "401 Unauthorized", "invalid credentials");
    return false;
}

static esp_err_t read_body(httpd_req_t *req, char *buffer, size_t capacity, size_t *out_len)
{
    if (req->content_len == 0 || req->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    *out_len = received;
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    const char *html = (const char *)_binary_index_html_start;
    size_t len = (size_t)(_binary_index_html_end - _binary_index_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, len);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    app_state_t st;
    app_state_get(&st);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(root, "ssid", st.active_ssid);
    cJSON_AddNumberToObject(root, "mode", (double)st.current_mode);
    cJSON_AddStringToObject(root, "mode_name", mode_name(st.current_mode));
    cJSON_AddNumberToObject(root, "connected_clients", (double)st.connected_clients);
    cJSON_AddStringToObject(root, "host_ip", st.host_ip);
    cJSON_AddBoolToObject(root, "proxy_active", st.proxy_active_on_host);
    cJSON_AddNumberToObject(root, "usb_rx", (double)st.usb_rx_frames);
    cJSON_AddNumberToObject(root, "usb_tx", (double)st.usb_tx_frames);
    cJSON_AddNumberToObject(root, "wifi_rx", (double)st.wifi_rx_frames);
    cJSON_AddNumberToObject(root, "wifi_tx", (double)st.wifi_tx_frames);
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    return send_json(req, root);
}

static esp_err_t handle_mode(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    char body[MB_WEB_BODY_MAX];
    size_t len = 0;
    if (read_body(req, body, sizeof(body), &len) != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_error(req, "400 Bad Request", "invalid json");
    }

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (!cJSON_IsNumber(mode) || mode->valuedouble != (double)mode->valueint ||
        mode->valueint < MODE_P2P_PROXY || mode->valueint > MODE_ENTERPRISE_STEALTH) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "mode must be 0, 1 or 2");
    }

    int requested = mode->valueint;
    cJSON_Delete(root);

    esp_err_t err = wifi_bridge_apply_mode((app_mode_t)requested);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply mode %d gagal: %s", requested, esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(reply, "status", "ok");
    cJSON_AddNumberToObject(reply, "active_mode", requested);
    return send_json(req, reply);
}

static bool json_get_bool(cJSON *root, const char *key, bool *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static bool json_get_int(cJSON *root, const char *key, int min, int max, int *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)item->valueint ||
        item->valueint < min || item->valueint > max) {
        return false;
    }
    *out = item->valueint;
    return true;
}

static bool json_get_string(cJSON *root, const char *key, char *out, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= capacity) {
        return false;
    }
    strlcpy(out, item->valuestring, capacity);
    return true;
}

static bool parse_mac(const char *text, uint8_t out[6])
{
    unsigned int parts[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x",
               &parts[0], &parts[1], &parts[2],
               &parts[3], &parts[4], &parts[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (parts[i] > 0xFF) {
            return false;
        }
        out[i] = (uint8_t)parts[i];
    }
    return true;
}

static int format_config_txt(char *buffer, size_t capacity, const sys_config_t *cfg)
{
    return snprintf(buffer, capacity,
                    "# Maryati Bridge configuration\n"
                    "use_static=%d\n"
                    "static_ssid=%s\n"
                    "static_pass=%s\n"
                    "default_mode=%d\n"
                    "enable_msc=%d\n"
                    "hidden_ssid=%d\n"
                    "tx_power_dbm=%d\n"
                    "custom_mac=%02X:%02X:%02X:%02X:%02X:%02X\n"
                    "usb_vid=0x%04X\n"
                    "usb_pid=0x%04X\n",
                    cfg->use_static_cred ? 1 : 0,
                    cfg->static_ssid,
                    cfg->static_pass,
                    (int)cfg->default_boot_mode,
                    cfg->enable_msc ? 1 : 0,
                    cfg->hidden_ssid ? 1 : 0,
                    (int)cfg->tx_power_dbm,
                    cfg->custom_mac[0], cfg->custom_mac[1], cfg->custom_mac[2],
                    cfg->custom_mac[3], cfg->custom_mac[4], cfg->custom_mac[5],
                    cfg->usb_vid, cfg->usb_pid);
}

static esp_err_t persist_config_txt(const sys_config_t *cfg)
{
    char buffer[MB_WEB_CONFIG_MAX];
    int written = format_config_txt(buffer, sizeof(buffer), cfg);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (usb_composite_msc_active()) {
        return usb_composite_msc_write_file("config.txt", buffer, (size_t)written);
    }
    return fs_healing_write_file("config.txt", buffer, (size_t)written);
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    sys_config_t cfg;
    app_state_get_config(&cfg);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             cfg.custom_mac[0], cfg.custom_mac[1], cfg.custom_mac[2],
             cfg.custom_mac[3], cfg.custom_mac[4], cfg.custom_mac[5]);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "use_static_cred", cfg.use_static_cred);
    cJSON_AddStringToObject(root, "static_ssid", cfg.static_ssid);
    cJSON_AddStringToObject(root, "static_pass", cfg.static_pass);
    cJSON_AddNumberToObject(root, "default_boot_mode", (double)cfg.default_boot_mode);
    cJSON_AddBoolToObject(root, "enable_msc", cfg.enable_msc);
    cJSON_AddBoolToObject(root, "hidden_ssid", cfg.hidden_ssid);
    cJSON_AddNumberToObject(root, "tx_power_dbm", (double)cfg.tx_power_dbm);
    cJSON_AddStringToObject(root, "custom_mac", mac);
    cJSON_AddNumberToObject(root, "usb_vid", (double)cfg.usb_vid);
    cJSON_AddNumberToObject(root, "usb_pid", (double)cfg.usb_pid);
    return send_json(req, root);
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    char body[MB_WEB_BODY_MAX];
    size_t len = 0;
    if (read_body(req, body, sizeof(body), &len) != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_error(req, "400 Bad Request", "invalid json");
    }

    sys_config_t cfg;
    app_state_get_config(&cfg);

    int boot_mode = (int)cfg.default_boot_mode;
    int tx_power = (int)cfg.tx_power_dbm;
    int usb_vid = (int)cfg.usb_vid;
    int usb_pid = (int)cfg.usb_pid;
    char mac_text[18] = { 0 };

    bool ok = json_get_bool(root, "use_static_cred", &cfg.use_static_cred) &&
              json_get_bool(root, "enable_msc", &cfg.enable_msc) &&
              json_get_bool(root, "hidden_ssid", &cfg.hidden_ssid) &&
              json_get_string(root, "static_ssid", cfg.static_ssid, sizeof(cfg.static_ssid)) &&
              json_get_string(root, "static_pass", cfg.static_pass, sizeof(cfg.static_pass)) &&
              json_get_string(root, "custom_mac", mac_text, sizeof(mac_text)) &&
              json_get_int(root, "default_boot_mode", 0, 2, &boot_mode) &&
              json_get_int(root, "tx_power_dbm", 8, 20, &tx_power) &&
              json_get_int(root, "usb_vid", 0, 0xFFFF, &usb_vid) &&
              json_get_int(root, "usb_pid", 0, 0xFFFF, &usb_pid);

    if (!ok) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "invalid field type or range");
    }

    cfg.default_boot_mode = (app_mode_t)boot_mode;
    cfg.tx_power_dbm = (int8_t)tx_power;
    cfg.usb_vid = (uint16_t)usb_vid;
    cfg.usb_pid = (uint16_t)usb_pid;

    if (mac_text[0] != '\0' && !parse_mac(mac_text, cfg.custom_mac)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "invalid custom_mac");
    }
    cJSON_Delete(root);

    size_t pass_len = strlen(cfg.static_pass);
    if ((cfg.use_static_cred && (cfg.static_ssid[0] == '\0' || pass_len < 8)) ||
        (pass_len != 0 && pass_len < 8)) {
        return send_error(req, "400 Bad Request", "static password must be 8-63 chars");
    }

    esp_err_t err = app_state_update_config(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "simpan NVS gagal: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "nvs save failed");
    }

    err = persist_config_txt(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tulis config.txt gagal: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "config.txt write failed");
    }

    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(reply, "status", "saved");
    return send_json(req, reply);
}

static esp_err_t do_rotate_credentials(char *ssid, size_t ssid_len,
                                       char *pass, size_t pass_len)
{
    app_state_generate_random_credentials(ssid, ssid_len, pass, pass_len);
    esp_err_t err = app_state_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }
    app_state_t st;
    app_state_get(&st);
    app_state_set_preview_menu(st.current_mode, false);
    return ESP_OK;
}

static void do_factory_reset(void)
{
    ESP_LOGW(TAG, "FACTORY RESET dari Web GUI");
    usb_composite_release_msc();
    esp_err_t err = fs_healing_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format FAT gagal: %s", esp_err_to_name(err));
    }
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hapus NVS gagal: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static esp_err_t handle_action(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    char body[MB_WEB_BODY_MAX];
    size_t len = 0;
    if (read_body(req, body, sizeof(body), &len) != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_error(req, "400 Bad Request", "invalid json");
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action) || action->valuestring == NULL) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "missing action");
    }

    bool rotate = strcmp(action->valuestring, "rotate_credentials") == 0;
    bool reset = strcmp(action->valuestring, "factory_reset") == 0;
    cJSON_Delete(root);

    if (!rotate && !reset) {
        return send_error(req, "400 Bad Request", "unknown action");
    }

    char ssid[MB_SSID_MAX_LEN];
    char pass[MB_PASS_MAX_LEN];
    if (rotate) {
        esp_err_t err = do_rotate_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rotate credentials gagal: %s", esp_err_to_name(err));
            return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
        }
    }

    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(reply, "status", "executed");
    esp_err_t err = send_json(req, reply);

    if (rotate) {
        /* Kirim respons dulu; ganti kredensial memutus klien Wi-Fi ini. */
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_err_t apply_err = wifi_bridge_apply_credentials(ssid, pass);
        if (apply_err != ESP_OK) {
            ESP_LOGE(TAG, "apply credentials gagal: %s", esp_err_to_name(apply_err));
        }
    }

    if (reset) {
        /* Beri waktu respons terkirim sebelum format + reboot. */
        vTaskDelay(pdMS_TO_TICKS(500));
        do_factory_reset();
    }
    return err;
}

static const httpd_uri_t s_uris[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = handle_root },
    { .uri = "/api/status",  .method = HTTP_GET,  .handler = handle_status },
    { .uri = "/api/mode",    .method = HTTP_POST, .handler = handle_mode },
    { .uri = "/api/config",  .method = HTTP_GET,  .handler = handle_config_get },
    { .uri = "/api/config",  .method = HTTP_POST, .handler = handle_config_post },
    { .uri = "/api/action",  .method = HTTP_POST, .handler = handle_action },
};

esp_err_t web_gui_start(void)
{
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = sizeof(s_uris) / sizeof(s_uris[0]);
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start gagal: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s gagal: %s", s_uris[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    ESP_LOGI(TAG, "Web GUI aktif di port 80");
    return ESP_OK;
}
