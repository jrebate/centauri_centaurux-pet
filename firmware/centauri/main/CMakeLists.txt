idf_component_register(SRCS "main.c" "font.c" "sprites.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_lcd_gc9a01 driver esp_wifi esp_netif esp_event esp_http_server nvs_flash json mdns esp_timer)
