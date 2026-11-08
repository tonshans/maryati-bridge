file(GLOB MB_APP_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.c")

idf_component_register(
    SRCS ${MB_APP_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_psram
        fatfs
        vfs
        esp_http_server
        esp_driver_gpio
        esp_driver_i2c
        esp_tinyusb
        tinyusb
        wear_levelling
        esp_partition
        cjson
        mbedtls
        rodata_assets
)
