#include "ui_oled.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_state.h"
#include "board_pins.h"
#include "button_input.h"

static const char *TAG = "ui_oled";

#define MB_OLED_WIDTH       128
#define MB_OLED_HEIGHT      64
#define MB_OLED_FB_SIZE     (MB_OLED_WIDTH * MB_OLED_HEIGHT / 8)
#define MB_OLED_LINE_H      16
#define MB_OLED_CHAR_W      6
#define MB_OLED_MAX_CHARS   (MB_OLED_WIDTH / MB_OLED_CHAR_W)

#define MB_OLED_DIM_SEC     120
#define MB_OLED_OFF_SEC     300
#define MB_OLED_DIM_CONTRAST 0x01
#define MB_OLED_ON_CONTRAST  0xCF

#define MB_OLED_I2C_TIMEOUT_MS 100

/* Font 5x7 (X11 Fixed/misc 5x7, public domain), ASCII 0x20..0x7E.
 * Setiap glyph = 5 kolom, bit0 = baris paling atas. */
static const uint8_t font5x7[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x2F, 0x00, 0x00 },
    { 0x00, 0x07, 0x00, 0x07, 0x00 },
    { 0x14, 0x3E, 0x14, 0x3E, 0x14 },
    { 0x04, 0x2A, 0x3E, 0x2A, 0x10 },
    { 0x13, 0x08, 0x04, 0x32, 0x00 },
    { 0x14, 0x2A, 0x14, 0x20, 0x00 },
    { 0x00, 0x00, 0x07, 0x00, 0x00 },
    { 0x00, 0x1E, 0x21, 0x00, 0x00 },
    { 0x00, 0x21, 0x1E, 0x00, 0x00 },
    { 0x00, 0x2A, 0x1C, 0x2A, 0x00 },
    { 0x08, 0x08, 0x3E, 0x08, 0x08 },
    { 0x00, 0x40, 0x30, 0x10, 0x00 },
    { 0x08, 0x08, 0x08, 0x08, 0x00 },
    { 0x00, 0x30, 0x30, 0x00, 0x00 },
    { 0x10, 0x08, 0x04, 0x02, 0x00 },
    { 0x00, 0x1E, 0x21, 0x1E, 0x00 },
    { 0x00, 0x22, 0x3F, 0x20, 0x00 },
    { 0x22, 0x31, 0x29, 0x26, 0x00 },
    { 0x11, 0x25, 0x25, 0x1B, 0x00 },
    { 0x0C, 0x0A, 0x3F, 0x08, 0x00 },
    { 0x17, 0x25, 0x25, 0x19, 0x00 },
    { 0x1E, 0x25, 0x25, 0x18, 0x00 },
    { 0x01, 0x31, 0x0D, 0x03, 0x00 },
    { 0x1A, 0x25, 0x25, 0x1A, 0x00 },
    { 0x06, 0x29, 0x29, 0x1E, 0x00 },
    { 0x00, 0x36, 0x36, 0x00, 0x00 },
    { 0x40, 0x36, 0x16, 0x00, 0x00 },
    { 0x00, 0x08, 0x14, 0x22, 0x00 },
    { 0x14, 0x14, 0x14, 0x14, 0x00 },
    { 0x00, 0x22, 0x14, 0x08, 0x00 },
    { 0x00, 0x02, 0x29, 0x06, 0x00 },
    { 0x1E, 0x21, 0x2D, 0x0E, 0x00 },
    { 0x3E, 0x09, 0x09, 0x3E, 0x00 },
    { 0x3F, 0x25, 0x25, 0x1A, 0x00 },
    { 0x1E, 0x21, 0x21, 0x12, 0x00 },
    { 0x3F, 0x21, 0x21, 0x1E, 0x00 },
    { 0x3F, 0x25, 0x25, 0x21, 0x00 },
    { 0x3F, 0x05, 0x05, 0x01, 0x00 },
    { 0x1E, 0x21, 0x29, 0x3A, 0x00 },
    { 0x3F, 0x04, 0x04, 0x3F, 0x00 },
    { 0x00, 0x21, 0x3F, 0x21, 0x00 },
    { 0x10, 0x20, 0x20, 0x1F, 0x00 },
    { 0x3F, 0x0C, 0x12, 0x21, 0x00 },
    { 0x3F, 0x20, 0x20, 0x20, 0x00 },
    { 0x3F, 0x06, 0x06, 0x3F, 0x00 },
    { 0x3F, 0x06, 0x18, 0x3F, 0x00 },
    { 0x1E, 0x21, 0x21, 0x1E, 0x00 },
    { 0x3F, 0x09, 0x09, 0x06, 0x00 },
    { 0x1E, 0x31, 0x21, 0x5E, 0x00 },
    { 0x3F, 0x09, 0x19, 0x26, 0x00 },
    { 0x12, 0x25, 0x29, 0x12, 0x00 },
    { 0x00, 0x01, 0x3F, 0x01, 0x00 },
    { 0x1F, 0x20, 0x20, 0x1F, 0x00 },
    { 0x0F, 0x30, 0x30, 0x0F, 0x00 },
    { 0x3F, 0x18, 0x18, 0x3F, 0x00 },
    { 0x33, 0x0C, 0x0C, 0x33, 0x00 },
    { 0x00, 0x07, 0x38, 0x07, 0x00 },
    { 0x31, 0x29, 0x25, 0x23, 0x00 },
    { 0x00, 0x3F, 0x21, 0x21, 0x00 },
    { 0x02, 0x04, 0x08, 0x10, 0x00 },
    { 0x00, 0x21, 0x21, 0x3F, 0x00 },
    { 0x00, 0x02, 0x01, 0x02, 0x00 },
    { 0x20, 0x20, 0x20, 0x20, 0x00 },
    { 0x00, 0x01, 0x02, 0x00, 0x00 },
    { 0x18, 0x24, 0x14, 0x3C, 0x00 },
    { 0x3F, 0x24, 0x24, 0x18, 0x00 },
    { 0x18, 0x24, 0x24, 0x00, 0x00 },
    { 0x18, 0x24, 0x24, 0x3F, 0x00 },
    { 0x18, 0x34, 0x2C, 0x08, 0x00 },
    { 0x08, 0x3E, 0x09, 0x02, 0x00 },
    { 0x28, 0x54, 0x54, 0x4C, 0x00 },
    { 0x3F, 0x04, 0x04, 0x38, 0x00 },
    { 0x00, 0x24, 0x3D, 0x20, 0x00 },
    { 0x00, 0x20, 0x40, 0x3D, 0x00 },
    { 0x3F, 0x08, 0x14, 0x20, 0x00 },
    { 0x00, 0x21, 0x3F, 0x20, 0x00 },
    { 0x3C, 0x08, 0x0C, 0x38, 0x00 },
    { 0x3C, 0x04, 0x04, 0x38, 0x00 },
    { 0x18, 0x24, 0x24, 0x18, 0x00 },
    { 0x7C, 0x24, 0x24, 0x18, 0x00 },
    { 0x18, 0x24, 0x24, 0x7C, 0x00 },
    { 0x3C, 0x04, 0x04, 0x08, 0x00 },
    { 0x28, 0x2C, 0x34, 0x14, 0x00 },
    { 0x04, 0x1F, 0x24, 0x20, 0x00 },
    { 0x1C, 0x20, 0x20, 0x3C, 0x00 },
    { 0x00, 0x1C, 0x20, 0x1C, 0x00 },
    { 0x3C, 0x30, 0x30, 0x3C, 0x00 },
    { 0x24, 0x18, 0x18, 0x24, 0x00 },
    { 0x0C, 0x50, 0x20, 0x1C, 0x00 },
    { 0x24, 0x34, 0x2C, 0x24, 0x00 },
    { 0x00, 0x04, 0x1E, 0x21, 0x00 },
    { 0x00, 0x00, 0x3F, 0x00, 0x00 },
    { 0x00, 0x21, 0x1E, 0x04, 0x00 },
    { 0x02, 0x01, 0x02, 0x01, 0x00 },
};

_Static_assert(sizeof(font5x7) / sizeof(font5x7[0]) == 95,
               "font5x7 must cover ASCII 0x20..0x7E");

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_task;

static uint8_t s_fb[MB_OLED_FB_SIZE];
static uint8_t s_tx[MB_OLED_FB_SIZE + 1];
static oled_power_state_t s_power = OLED_STATE_ACTIVE;
static bool s_cleared;

static uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static esp_err_t oled_cmd(const uint8_t *cmds, size_t len)
{
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 0x00;
    memcpy(buf + 1, cmds, len);
    return i2c_master_transmit(s_dev, buf, len + 1, MB_OLED_I2C_TIMEOUT_MS);
}

static esp_err_t oled_flush(void)
{
    static const uint8_t window[] = { 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    esp_err_t err = oled_cmd(window, sizeof(window));
    if (err != ESP_OK) {
        return err;
    }
    s_tx[0] = 0x40;
    memcpy(s_tx + 1, s_fb, sizeof(s_fb));
    return i2c_master_transmit(s_dev, s_tx, sizeof(s_tx), MB_OLED_I2C_TIMEOUT_MS);
}

static void oled_apply_power(oled_power_state_t state)
{
    esp_err_t err;
    switch (state) {
    case OLED_STATE_OFF: {
        static const uint8_t off[] = { 0xAE };
        err = oled_cmd(off, sizeof(off));
        break;
    }
    case OLED_STATE_DIMMED: {
        static const uint8_t dim[] = { 0xAF, 0x81, MB_OLED_DIM_CONTRAST };
        err = oled_cmd(dim, sizeof(dim));
        break;
    }
    case OLED_STATE_ACTIVE:
    default: {
        static const uint8_t on[] = { 0xAF, 0x81, MB_OLED_ON_CONTRAST };
        err = oled_cmd(on, sizeof(on));
        break;
    }
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "perintah power OLED gagal: %s", esp_err_to_name(err));
    }
}

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_char(int x, int y, char ch)
{
    uint8_t c = (uint8_t)ch;
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        int px = x + col;
        if (px < 0 || px >= MB_OLED_WIDTH) {
            continue;
        }
        for (int row = 0; row < 7; row++) {
            if ((bits & (1u << row)) == 0) {
                continue;
            }
            int py = y + row;
            if (py < 0 || py >= MB_OLED_HEIGHT) {
                continue;
            }
            s_fb[(py / 8) * MB_OLED_WIDTH + px] |= (uint8_t)(1u << (py % 8));
        }
    }
}

static void fb_text(int x, int y, const char *text)
{
    for (size_t i = 0; text[i] != '\0' && x < MB_OLED_WIDTH; i++) {
        fb_char(x, y, text[i]);
        x += MB_OLED_CHAR_W;
    }
}

static const char *mode_title(app_mode_t mode)
{
    switch (mode) {
    case MODE_TRANSPARENT:
        return "BRIDGED";
    case MODE_ENTERPRISE_STEALTH:
        return "STEALTH";
    case MODE_P2P_PROXY:
    default:
        return "NoBridge";
    }
}

static const char *menu_title(mb_menu_item_t item)
{
    switch (item) {
    case MB_MENU_MODE_0:
        return "> Mode 0: P2P Proxy";
    case MB_MENU_MODE_1:
        return "> Mode 1: Transparent";
    case MB_MENU_MODE_2:
        return "> Mode 2: Stealth";
    case MB_MENU_ROTATE:
    default:
        return "> Rotate WiFi Pass";
    }
}

static void oled_render(const app_state_t *st)
{
    char line[96];

    fb_clear();

    snprintf(line, sizeof(line), "M%d:%-8s [C:%u/%d]",
             (int)st->current_mode, mode_title(st->current_mode),
             (unsigned)st->connected_clients, MB_MAX_WIFI_CLIENTS);
    fb_text(0, 0, line);

    if (st->current_mode == MODE_ENTERPRISE_STEALTH) {
        snprintf(line, sizeof(line), "[HIDDEN] %s", st->active_ssid);
    } else {
        snprintf(line, sizeof(line), "SSID: %s", st->active_ssid);
    }
    fb_text(0, MB_OLED_LINE_H, line);

    snprintf(line, sizeof(line), "PASS: %s", st->active_password);
    fb_text(0, MB_OLED_LINE_H * 2, line);

    if (st->menu_selection_active) {
        fb_text(0, MB_OLED_LINE_H * 3, menu_title(button_input_get_menu_item()));
    } else if (st->proxy_active_on_host) {
        snprintf(line, sizeof(line), "Proxy: %s", st->host_ip);
        fb_text(0, MB_OLED_LINE_H * 3, line);
    } else {
        snprintf(line, sizeof(line), "Host: %s", st->host_ip);
        fb_text(0, MB_OLED_LINE_H * 3, line);
    }

    esp_err_t err = oled_flush();
    if (err != ESP_OK && !s_cleared) {
        ESP_LOGW(TAG, "flush OLED gagal: %s (panel terpasang?)", esp_err_to_name(err));
        s_cleared = true;
    }
}

static void ui_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(200));

        app_state_t st;
        app_state_get(&st);

        uint32_t idle = now_sec() - st.last_input_timestamp_sec;
        oled_power_state_t want = OLED_STATE_ACTIVE;
        if (idle >= MB_OLED_OFF_SEC) {
            want = OLED_STATE_OFF;
        } else if (idle >= MB_OLED_DIM_SEC) {
            want = OLED_STATE_DIMMED;
        }

        if (st.oled_state != want) {
            app_state_set_oled_state(want);
        }
        if (want != s_power) {
            oled_apply_power(want);
            s_power = want;
        }

        if (want != OLED_STATE_OFF) {
            oled_render(&st);
        }
    }
}

esp_err_t ui_oled_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = MB_OLED_I2C_PORT,
        .sda_io_num = MB_OLED_SDA_GPIO,
        .scl_io_num = MB_OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus I2C gagal: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MB_OLED_I2C_ADDR,
        .scl_speed_hz = MB_OLED_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tambah device OLED gagal: %s", esp_err_to_name(err));
        return err;
    }

    static const uint8_t init_seq[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide */
        0xA8, 0x3F,       /* multiplex 64 */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x00,       /* horizontal addressing */
        0xA1,             /* segment remap */
        0xC8,             /* COM scan dec */
        0xDA, 0x12,       /* COM pins */
        0x81, MB_OLED_ON_CONTRAST,
        0xD9, 0xF1,       /* precharge */
        0xDB, 0x40,       /* VCOM detect */
        0xA4,             /* resume RAM content */
        0xA6,             /* normal (not inverted) */
        0x2E,             /* deactivate scroll */
        0xAF,             /* display on */
    };
    err = oled_cmd(init_seq, sizeof(init_seq));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init SSD1306 gagal: %s (lanjut tanpa panel)", esp_err_to_name(err));
    }

    fb_clear();
    (void)oled_flush();
    s_power = OLED_STATE_ACTIVE;

    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "mb_oled", 4096, NULL, 3, &s_task, MB_TASK_CORE_APP);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OLED siap (SDA=%d SCL=%d addr=0x%02X)",
             MB_OLED_SDA_GPIO, MB_OLED_SCL_GPIO, MB_OLED_I2C_ADDR);
    return ESP_OK;
}
