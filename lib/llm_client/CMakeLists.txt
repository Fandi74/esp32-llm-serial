idf_component_register(SRCS "llm_client.c"
                     INCLUDE_DIRS "."
                     REQUIRES esp_http_client esp_netif cjson)
