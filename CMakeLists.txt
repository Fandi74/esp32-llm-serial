cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-llm-serial)

idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "llm_client.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_tls
        cjson
        log
)