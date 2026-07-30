idf_component_register(
    SRCS "e2e_main.c"
    INCLUDE_DIRS "."
    REQUIRES convai_ws esp_websocket_client cjson esp_event esp_timer
)
