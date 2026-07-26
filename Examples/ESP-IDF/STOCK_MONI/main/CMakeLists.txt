idf_component_register(SRCS "app_main.c" "lcd.c" "touch.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_timer driver nvs_flash esp_wifi esp_netif esp_event esp_lvgl_port lvgl esp_driver_ledc esp_driver_spi esp_driver_gpio)
