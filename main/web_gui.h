#pragma once

#include "esp_err.h"

/* Nyalakan HTTP server port 80: index.html dan endpoint /api (Bearer/Basic
 * auth memakai password Wi-Fi aktif). */
esp_err_t web_gui_start(void);
