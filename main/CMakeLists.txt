idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES convai_ws aitalk esp_wifi esp_event nvs_flash esp_netif cjson
)
