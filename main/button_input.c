#include "button_input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "app_state.h"
#include "board_pins.h"
#include "fs_healing.h"
#include "usb_composite.h"
#include "wifi_bridge.h"

static const char *TAG = "button";

#define MB_BTN_COUNT            3
#define MB_BTN_DEBOUNCE_SAMPLES 3
#define MB_BTN_POLL_MS          20
#define MB_BTN_SHORT_MS         500
#define MB_BTN_LONG_MS          2000
#define MB_BTN_RESET_MS         10000
#define MB_BTN_COMBO_RESET_MS   5000

enum {
    BTN_SELECT = 0,
    BTN_UP = 1,
    BTN_DOWN = 2,
};

typedef struct {
    bool stable;
    bool last_raw;
    uint8_t count;
} debounce_t;

static const gpio_num_t s_pins[MB_BTN_COUNT] = {
    MB_BTN_SELECT_GPIO,
    MB_BTN_UP_GPIO,
    MB_BTN_DOWN_GPIO,
};

static debounce_t s_deb[MB_BTN_COUNT];
static bool s_prev[MB_BTN_COUNT];
static bool s_pressed[MB_BTN_COUNT];
static bool s_ignore_release[MB_BTN_COUNT];
static uint32_t s_press_ms[MB_BTN_COUNT];

static bool s_suppress_actions;   /* tekan saat OLED redup/mati = wake saja */
static bool s_long_applied;
static bool s_combo;
static bool s_combo_fired;
#if MB_BUTTON_SCHEME != 1
static uint32_t s_combo_ms;
#endif

static volatile mb_menu_item_t s_item;
static TaskHandle_t s_task;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool btn_raw(gpio_num_t pin)
{
    return gpio_get_level(pin) == MB_BTN_ACTIVE_LEVEL;
}

static void debounce_update(debounce_t *d, bool raw)
{
    if (raw != d->last_raw) {
        d->last_raw = raw;
        d->count = 0;
        return;
    }
    if (d->count < MB_BTN_DEBOUNCE_SAMPLES) {
        d->count++;
        if (d->count == MB_BTN_DEBOUNCE_SAMPLES) {
            d->stable = raw;
        }
    }
}

static void show_preview(void)
{
    app_state_t st;
    app_state_get(&st);

    app_mode_t preview = st.current_mode;
    if (s_item <= MB_MENU_MODE_2) {
        preview = (app_mode_t)s_item;
    }
    app_state_set_preview_menu(preview, true);
}

static void cycle_item(int direction)
{
    int value = (int)s_item + direction;
    if (value < 0) {
        value = MB_MENU_COUNT - 1;
    } else if (value >= MB_MENU_COUNT) {
        value = 0;
    }
    s_item = (mb_menu_item_t)value;
    show_preview();
    ESP_LOGI(TAG, "menu item %d", (int)s_item);
}

static void rotate_credentials(void)
{
    char ssid[MB_SSID_MAX_LEN];
    char pass[MB_PASS_MAX_LEN];

    app_state_generate_random_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (app_state_set_credentials(ssid, pass) != ESP_OK) {
        ESP_LOGE(TAG, "gagal menyimpan kredensial baru");
        return;
    }

    esp_err_t err = wifi_bridge_apply_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gagal menerapkan kredensial: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "kredensial diputar: %s", ssid);
    }

    app_state_t st;
    app_state_get(&st);
    app_state_set_preview_menu(st.current_mode, false);
}

static void apply_item(void)
{
    if (s_item == MB_MENU_ROTATE) {
        rotate_credentials();
        return;
    }

    app_mode_t mode = (app_mode_t)s_item;
    esp_err_t err = wifi_bridge_apply_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gagal menerapkan mode %d: %s", (int)mode, esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "mode %d diterapkan dari tombol", (int)mode);
    }
}

static void short_action(int button)
{
    switch (MB_BUTTON_SCHEME) {
    case 1:
        if (button == BTN_SELECT) {
            cycle_item(1);
        }
        break;
    case 2:
        if (button == BTN_UP) {
            cycle_item(1);
        } else if (button == BTN_SELECT) {
            apply_item();
        }
        break;
    case 3:
        if (button == BTN_UP) {
            cycle_item(-1);
        } else if (button == BTN_DOWN) {
            cycle_item(1);
        } else if (button == BTN_SELECT) {
            apply_item();
        }
        break;
    default:
        break;
    }
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "FACTORY RESET: format FAT + hapus NVS");

    /* MSC memegang wear levelling; lepas dulu sebelum format. */
    usb_composite_release_msc();
    esp_err_t err = fs_healing_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset FAT gagal: %s", esp_err_to_name(err));
    }
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hapus NVS gagal: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void button_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MB_BTN_POLL_MS));

        uint32_t now = now_ms();
        bool cur[MB_BTN_COUNT] = { false };
        for (int i = 0; i < MB_BTN_COUNT; i++) {
            debounce_update(&s_deb[i], btn_raw(s_pins[i]));
            cur[i] = s_deb[i].stable;
        }

        /* Tepi tekan: wake layar + catat awal durasi. */
        for (int i = 0; i < MB_BTN_COUNT; i++) {
            if (cur[i] && !s_prev[i]) {
                if (!s_pressed[BTN_SELECT] && !s_pressed[BTN_UP] && !s_pressed[BTN_DOWN]) {
                    app_state_t st;
                    app_state_get(&st);
                    if (st.oled_state != OLED_STATE_ACTIVE) {
                        s_suppress_actions = true;
                    }
                }
                app_state_record_button_activity();
                s_pressed[i] = true;
                s_ignore_release[i] = false;
                s_press_ms[i] = now;
            }
        }

        bool sel = cur[BTN_SELECT];
        bool up = cur[BTN_UP];
        bool down = cur[BTN_DOWN];
        bool any_now = sel || up || down;

#if MB_BUTTON_SCHEME == 1
        if (sel && s_pressed[BTN_SELECT] && !s_suppress_actions) {
            uint32_t held = now - s_press_ms[BTN_SELECT];
            if (held >= MB_BTN_RESET_MS && !s_combo_fired) {
                s_combo_fired = true;
                factory_reset();
            } else if (held >= MB_BTN_LONG_MS && !s_long_applied) {
                s_long_applied = true;
                apply_item();
            }
        }
#else
        bool up_select = up && sel;
#if MB_BUTTON_SCHEME == 3
        bool up_down = up && down;
        bool combo = up_select || up_down;
#else
        bool combo = up_select;
#endif

        if (combo && !s_suppress_actions) {
            if (!s_combo) {
                s_combo = true;
                s_combo_ms = now;
#if MB_BUTTON_SCHEME == 3
                if (up_down && !up_select) {
                    rotate_credentials();
                    s_combo_fired = true;
                }
#endif
            }
            for (int i = 0; i < MB_BTN_COUNT; i++) {
                if (cur[i]) {
                    s_ignore_release[i] = true;
                }
            }
#if MB_BUTTON_SCHEME == 2
            if (now - s_combo_ms >= MB_BTN_COMBO_RESET_MS && !s_combo_fired) {
                s_combo_fired = true;
                factory_reset();
            }
#else
            if (up_select && now - s_combo_ms >= MB_BTN_COMBO_RESET_MS && !s_combo_fired) {
                s_combo_fired = true;
                factory_reset();
            }
#endif
        } else if (!combo) {
            s_combo = false;
        }
#endif /* MB_BUTTON_SCHEME == 1 */

        /* Tepi lepas: aksi short press bila bukan wake-only / bagian combo. */
        for (int i = 0; i < MB_BTN_COUNT; i++) {
            if (!cur[i] && s_prev[i]) {
                uint32_t held = now - s_press_ms[i];
                if (s_pressed[i] && !s_suppress_actions && !s_ignore_release[i] &&
                    held < MB_BTN_SHORT_MS) {
                    short_action(i);
                }
                s_pressed[i] = false;
            }
        }

        if (!any_now) {
            s_suppress_actions = false;
            s_long_applied = false;
            s_combo = false;
            s_combo_fired = false;
            memset(s_ignore_release, 0, sizeof(s_ignore_release));
        }

        memcpy(s_prev, cur, sizeof(s_prev));
    }
}

esp_err_t button_input_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t mask = 0;
    for (int i = 0; i < MB_BTN_COUNT; i++) {
        mask |= (1ULL << s_pins[i]);
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "konfigurasi GPIO gagal: %s", esp_err_to_name(err));
        return err;
    }

    memset(s_deb, 0, sizeof(s_deb));
    for (int i = 0; i < MB_BTN_COUNT; i++) {
        s_deb[i].last_raw = btn_raw(s_pins[i]);
        s_deb[i].stable = s_deb[i].last_raw;
        s_deb[i].count = MB_BTN_DEBOUNCE_SAMPLES;
        s_prev[i] = s_deb[i].stable;
    }
    memset(s_pressed, 0, sizeof(s_pressed));
    memset(s_ignore_release, 0, sizeof(s_ignore_release));

    app_state_t st;
    app_state_get(&st);
    s_item = (st.current_mode <= MODE_ENTERPRISE_STEALTH)
                 ? (mb_menu_item_t)st.current_mode
                 : MB_MENU_MODE_0;

    BaseType_t ok = xTaskCreatePinnedToCore(button_task, "mb_button", 3072, NULL, 4,
                                            &s_task, MB_TASK_CORE_APP);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "tombol siap (scheme %d)", MB_BUTTON_SCHEME);
    return ESP_OK;
}

mb_menu_item_t button_input_get_menu_item(void)
{
    return s_item;
}
