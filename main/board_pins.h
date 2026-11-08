#pragma once

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"

/*
 * Hardware mapping for the two supported boards.
 * Pin choices live here only; every other module includes this header.
 */

#if CONFIG_IDF_TARGET_ESP32S3

#define MB_OLED_SDA_GPIO        8
#define MB_OLED_SCL_GPIO        9
#define MB_BTN_SELECT_GPIO      0
#define MB_BTN_UP_GPIO          4
#define MB_BTN_DOWN_GPIO        5

/* Dual core: USB/L2 TX on core 1, Wi-Fi/Web/OLED on core 0. */
#define MB_TASK_CORE_USB        1
#define MB_TASK_CORE_APP        0

#elif CONFIG_IDF_TARGET_ESP32S2

#define MB_OLED_SDA_GPIO        33
#define MB_OLED_SCL_GPIO        35
#define MB_BTN_SELECT_GPIO      0
#define MB_BTN_UP_GPIO          1
#define MB_BTN_DOWN_GPIO        2

/* Single core: no affinity. */
#define MB_TASK_CORE_USB        tskNO_AFFINITY
#define MB_TASK_CORE_APP        tskNO_AFFINITY

#else
#error "Maryati Bridge supports only ESP32-S3 and ESP32-S2"
#endif

#define MB_OLED_I2C_PORT        0
#define MB_OLED_I2C_FREQ_HZ     400000
#define MB_OLED_I2C_ADDR        0x3C

/* Buttons wire to GND, internal pull-up. */
#define MB_BTN_ACTIVE_LEVEL     0

#define MB_BUTTON_SCHEME        CONFIG_MB_BUTTON_SCHEME

#if MB_BUTTON_SCHEME == 1
#define MB_HAS_UP_DOWN_BUTTONS  0
#else
#define MB_HAS_UP_DOWN_BUTTONS  1
#endif
