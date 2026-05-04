#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "llm_client.h"
#include "api.h"          /* credentials — git-ignored, never committed */

#define TAG "main"

#define WIFI_MAX_RETRY 5

/* ------------------------------------------------------------------ */
/* Serial REPL task                                                     */
/* ------------------------------------------------------------------ */

static void repl_task(void *pvParameters)
{
    /* Input buffer lives on the heap — 1 KB is plenty for a typed line */
    char *input = malloc(LLM_MAX_PROMPT_LEN);

    /* Reply buffer in PSRAM (falls back to internal RAM if unavailable) */
    char *reply = heap_caps_malloc(LLM_MAX_RESPONSE_LEN,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        reply = malloc(LLM_MAX_RESPONSE_LEN);
    }

    if (!input || !reply) {
        ESP_LOGE(TAG, "Failed to allocate REPL buffers — aborting");
        free(input);
        free(reply);
        vTaskDelete(NULL);
        return;
    }

    printf("\r\n");
    printf("========================================\r\n");
    printf("  ESP32-S3 LLM Serial Chat\r\n");
    printf("  Model : %s\r\n", OPENROUTER_MODEL);
    printf("  Type a message and press Enter.\r\n");
    printf("  Type 'exit' to quit.\r\n");
    printf("========================================\r\n\r\n");

    while (1) {
        printf("You: ");
        fflush(stdout);

        /* fgets blocks until the user presses Enter.
         * ESP-IDF maps UART0 / USB-Serial-JTAG to stdin automatically. */
        if (!fgets(input, LLM_MAX_PROMPT_LEN, stdin)) {
            /* EOF or read error — wait and retry */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Strip trailing \r and \n */
        size_t len = strlen(input);
        while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r')) {
            input[--len] = '\0';
        }

        /* Skip blank lines */
        if (len == 0) {
            continue;
        }

        if (strcmp(input, "exit") == 0) {
            printf("Goodbye!\r\n");
            break;
        }

        printf("Assistant (%s): thinking...\r\n", OPENROUTER_MODEL);

        esp_err_t ret = llm_chat(
            OPENROUTER_API_KEY,
            OPENROUTER_MODEL,
            input,
            reply,
            LLM_MAX_RESPONSE_LEN);

        if (ret == ESP_OK) {
            printf("\r\nAssistant: %s\r\n\r\n", reply);
        } else {
            printf("\r\n[Error: %s]\r\n\r\n", esp_err_to_name(ret));
        }
    }

    free(input);
    free(reply);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* app_main                                                            */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. TCP/IP stack and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Wi-Fi — block until IP obtained */
    ret = wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_MAX_RETRY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi failed — cannot reach OpenRouter");
        return;
    }

    /* 4. Start the serial REPL on core 0
     *    Give it 6 KB — fgets + cJSON + TLS callbacks need headroom   */
    xTaskCreatePinnedToCore(
        repl_task,
        "repl_task",
        6144,
        NULL,
        5,
        NULL,
        0);
}