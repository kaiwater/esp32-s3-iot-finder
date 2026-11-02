idf_component_register(
    SRCS "main.c" "finder.c" "st7735.c" "nrf24.c" "nrf_aux.c" "oui.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_lcd_st7735
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        esp_wifi
        nvs_flash
)
