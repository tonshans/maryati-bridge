#pragma once

#include "esp_err.h"

/* Item menu yang bisa dipilih dari tombol fisik. */
typedef enum {
    MB_MENU_MODE_0 = 0,
    MB_MENU_MODE_1,
    MB_MENU_MODE_2,
    MB_MENU_ROTATE,
    MB_MENU_COUNT
} mb_menu_item_t;

/* Baca GPIO tombol sesuai CONFIG_MB_BUTTON_SCHEME dan mulai task polling 20 ms. */
esp_err_t button_input_init(void);

/* Item yang sedang ditunjuk (dipakai ui_oled untuk baris menu). */
mb_menu_item_t button_input_get_menu_item(void);
