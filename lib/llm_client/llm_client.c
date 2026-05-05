#include "llm_client.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#define TAG               "llm_client"
#define OPENROUTER_URL    "https://openrouter.ai/api/v1/chat/completions"
#define HTTP_TIMEOUT_MS   30000
#define AUTH_HDR_PREFIX   "Bearer "

/* ------------------------------------------------------------------ */
/* HTTP event handler — accumulates chunked response body              */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t buf_len;
    size_t written;
    bool   overflow;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (ctx->overflow) return ESP_OK;
        if (ctx->written + evt->data_len >= ctx->buf_len - 1) {
            ESP_LOGW(TAG, "Response buffer overflow — truncating");
            ctx->overflow = true;
            return ESP_OK;
        }
        memcpy(ctx->buf + ctx->written, evt->data, evt->data_len);
        ctx->written += evt->data_len;
        ctx->buf[ctx->written] = '\0';
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Build JSON request body                                              */
/* ------------------------------------------------------------------ */

static char *build_request_json(const char *model, const char *prompt)
{
    cJSON *root     = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg      = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens", 512);
    cJSON_AddStringToObject(msg, "role",    "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/* ------------------------------------------------------------------ */
/* Parse assistant reply from response JSON                             */
/* ------------------------------------------------------------------ */

static esp_err_t parse_response(const char *json_str,
                                 char *out_buf, size_t out_len)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    /* Validate choices array exists and is an array */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!choices || !cJSON_IsArray(choices)) {
        ESP_LOGE(TAG, "Invalid or missing 'choices' in response");
        cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(msg))
            ESP_LOGE(TAG, "API error: %s", msg->valuestring);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate first choice exists */
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice) {
        ESP_LOGE(TAG, "No choices in response array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate message object exists */
    cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (!message) {
        ESP_LOGE(TAG, "Missing 'message' in choice");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate content exists and is a string */
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsString(content)) {
        ESP_LOGE(TAG, "Invalid or missing 'content' in message");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(out_buf, content->valuestring, out_len);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t llm_chat(const char *api_key, const char *model,
                   const char *user_prompt,
                   char *out_buf, size_t out_buf_len)
{
    if (!api_key || !model || !user_prompt || !out_buf || !out_buf_len)
        return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_FAIL;

    char *req_body = build_request_json(model, user_prompt);
    if (!req_body) return ESP_ERR_NO_MEM;

    size_t raw_len = out_buf_len * 3;
    char  *raw_buf = heap_caps_malloc(raw_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw_buf) raw_buf = malloc(raw_len);
    if (!raw_buf) { free(req_body); return ESP_ERR_NO_MEM; }
    raw_buf[0] = '\0';

    size_t auth_len = strlen(AUTH_HDR_PREFIX) + strlen(api_key) + 1;
    char  *auth_hdr = malloc(auth_len);
    if (!auth_hdr) { free(req_body); free(raw_buf); return ESP_ERR_NO_MEM; }
    snprintf(auth_hdr, auth_len, "%s%s", AUTH_HDR_PREFIX, api_key);

    http_ctx_t ctx = { .buf = raw_buf, .buf_len = raw_len, .written = 0, .overflow = false };

    esp_http_client_config_t config = {
        .url               = OPENROUTER_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ret = ESP_FAIL; goto cleanup; }

    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "HTTP-Referer",  "https://esp32-llm-serial.local");
    esp_http_client_set_header(client, "X-Title",       "ESP32-S3 LLM Serial");
    esp_http_client_set_post_field(client, req_body, (int)strlen(req_body));

    ESP_LOGI(TAG, "POST → OpenRouter [%s]", model);
    esp_err_t http_ret = esp_http_client_perform(client);

    if (http_ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(http_ret));
        esp_http_client_cleanup(client);
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %s", status, raw_buf);
        goto cleanup;
    }

    ret = parse_response(raw_buf, out_buf, out_buf_len);

cleanup:
    free(auth_hdr);
    free(raw_buf);
    free(req_body);
    return ret;
}