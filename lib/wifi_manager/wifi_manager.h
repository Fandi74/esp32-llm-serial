#pragma once

#include "esp_err.h"

/**
 * Initialise Wi-Fi in station mode and block until IP is obtained or
 * the retry limit is reached.
 */
esp_err_t wifi_manager_connect(const char *ssid,
                               const char *password,
                               int         max_retry);

/**
 * Disconnect and free Wi-Fi resources.
 */
void wifi_manager_disconnect(void);