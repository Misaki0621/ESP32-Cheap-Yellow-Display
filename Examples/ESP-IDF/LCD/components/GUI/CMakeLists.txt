idf_component_register(SRCS "gui.c"
                       INCLUDE_DIRS "." "include"
                       REQUIRES LCD esp_driver_gpio esp_driver_spi)