cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e2e_tests)

# esp_websocket_client is not warning-clean on the linux target
# (portMAX_DELAY conversion overflows with -Werror=overflow)
idf_component_get_property(ws_lib espressif__esp_websocket_client COMPONENT_LIB)
target_compile_options(${ws_lib} PRIVATE -Wno-error=overflow -Wno-error=conversion)
