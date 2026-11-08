#pragma once

#include "esp_err.h"

/* Nyalakan bus I2C + SSD1306 128x64 dan mulai task render 5 Hz.
 * Kegagalan panel tidak memblokir boot: task tetap berjalan dan mencoba
 * mengirim ulang setiap frame. */
esp_err_t ui_oled_init(void);
