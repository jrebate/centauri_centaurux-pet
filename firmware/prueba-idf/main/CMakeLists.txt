idf_component_register(SRCS "main.c" "font.c" "sprites.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_lcd_gc9a01 driver)
