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
#define LLM_CHAT_TIMEOUT_MS 60000  /* Overall operation timeout */

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
            ESP_LOGW(TAG, "Response buffer overflow — truncating (written=%u, incoming=%u, buf_len=%u)",
                     ctx->written, evt->data_len, ctx->buf_len);
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

    if (!root || !messages || !msg) {
        ESP_LOGE(TAG, "Failed to allocate cJSON objects for request");
        goto fail;
    }

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens", 512);
    cJSON_AddStringToObject(msg, "role",    "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;

fail:
    if (msg) cJSON_Delete(msg);
    if (messages) cJSON_Delete(messages);
    if (root) cJSON_Delete(root);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Parse assistant reply from response JSON                             */
/* ------------------------------------------------------------------ */

static esp_err_t parse_response(const char *json_str,
                                 char *out_buf, size_t out_len)
{
    if (!json_str || json_str[0] == '\0') {
        ESP_LOGE(TAG, "Empty response body — cannot parse JSON");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed — invalid JSON: %s", json_str);
        return ESP_FAIL;
    }

    /* Check for API-level error object (e.g., 429, 401, 400) */
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error) {
        cJSON *err_msg = cJSON_GetObjectItemCaseSensitive(error, "message");
        cJSON *err_code = cJSON_GetObjectItemCaseSensitive(error, "code");
        if (cJSON_IsString(err_msg)) {
            ESP_LOGE(TAG, "API error: %s (code: %s)",
                     err_msg->valuestring,
                     err_code ? err_code->valuestring : "unknown");
        } else {
            ESP_LOGE(TAG, "API error object present but no message");
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate choices array exists and is an array */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!choices || !cJSON_IsArray(choices)) {
        ESP_LOGE(TAG, "Invalid or missing 'choices' array in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate first choice exists */
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice) {
        ESP_LOGE(TAG, "No choices in response array (empty choices)");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate message object exists */
    cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (!message) {
        ESP_LOGE(TAG, "Missing 'message' object in first choice");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Validate content exists and is a string */
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsString(content)) {
        ESP_LOGE(TAG, "Invalid or missing 'content' string in message");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Copy content with truncation check */
    size_t content_len = strlen(content->valuestring);
    if (content_len >= out_len) {
        ESP_LOGW(TAG, "Content truncated: %u bytes available, %u bytes needed",
                 out_len, content_len + 1);
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
    if (!api_key || !model || !user_prompt || !out_buf || out_buf_len == 0) {
        ESP_LOGE(TAG, "Invalid argument: api_key=%p, model=%p, prompt=%p, out_buf=%p, out_buf_len=%u",
                 api_key, model, user_prompt, out_buf, out_buf_len);
        return ESP_ERR_INVALID_ARG;
    }

    if (out_buf_len > LLM_MAX_RESPONSE_LEN) {
        ESP_LOGW(TAG, "out_buf_len (%u) exceeds LLM_MAX_RESPONSE_LEN (%u)",
                 out_buf_len, LLM_MAX_RESPONSE_LEN);
    }

    esp_err_t ret = ESP_FAIL;
    TickType_t start_tick = xTaskGetTickCount();

    char *req_body = build_request_json(model, user_prompt);
    if (!req_body) {
        ESP_LOGE(TAG, "Failed to build request JSON (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    size_t raw_len = out_buf_len * 3;
    char  *raw_buf = malloc(raw_len);
    if (!raw_buf) {
        ESP_LOGE(TAG, "Failed to allocate raw_buf (%u bytes)", raw_len);
        free(req_body);
        return ESP_ERR_NO_MEM;
    }
    raw_buf[0] = '\0';

    size_t auth_len = strlen(AUTH_HDR_PREFIX) + strlen(api_key) + 1;
    char  *auth_hdr = malloc(auth_len);
    if (!auth_hdr) {
        ESP_LOGE(TAG, "Failed to allocate auth_hdr (%u bytes)", auth_len);
        free(req_body);
        free(raw_buf);
        return ESP_ERR_NO_MEM;
    }
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
    configASSERT(client != NULL);  /* Static assertion for production */

    esp_err_t set_hdr_ret;
    set_hdr_ret = esp_http_client_set_header(client, "Content-Type",  "application/json");
    ESP_ERROR_CHECK(set_hdr_ret);
    set_hdr_ret = esp_http_client_set_header(client, "Authorization", auth_hdr);
    ESP_ERROR_CHECK(set_hdr_ret);
    set_hdr_ret = esp_http_client_set_header(client, "HTTP-Referer",  "https://esp32-llm-serial.local");
    ESP_ERROR_CHECK(set_hdr_ret);
    set_hdr_ret = esp_http_client_set_header(client, "X-Title",       "ESP32-S3 LLM Serial");
    ESP_ERROR_CHECK(set_hdr_ret);

    esp_http_client_set_post_field(client, req_body, (int)strlen(req_body));

    ESP_LOGI(TAG, "POST → OpenRouter [%s] (timeout %ums)", model, HTTP_TIMEOUT_MS);
    esp_err_t http_ret = esp_http_client_perform(client);

    if (http_ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP transport error: %s", esp_err_to_name(http_ret));
        esp_http_client_cleanup(client);
        goto cleanup;
    }

    /* Check overall operation timeout */
    TickType_t elapsed = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
    if (elapsed > LLM_CHAT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "llm_chat operation timeout after %ums (limit %ums)", elapsed, LLM_CHAT_TIMEOUT_MS);
        /* Continue to process response, but log warning */
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %s", status, raw_buf);
        /* Provide hints for common status codes */
        switch (status) {
            case 401: ESP_LOGE(TAG, "Hint: Check API key validity"); break;
            case 429: ESP_LOGE(TAG, "Hint: Rate limited — reduce request frequency"); break;
            case 400: ESP_LOGE(TAG, "Hint: Bad request — check model name and prompt"); break;
            case 500: ESP_LOGE(TAG, "Hint: Server internal error — retry later"); break;
            default: break;
        }
        goto cleanup;
    }

    /* Check for buffer overflow during accumulation */
    if (ctx.overflow) {
        ESP_LOGE(TAG, "Response exceeded buffer size (%u bytes) — data truncated", raw_len);
        goto cleanup;
    }

    /* Check for empty response */
    if (ctx.written == 0) {
        ESP_LOGE(TAG, "Empty response body from API (HTTP 200 but no data)");
        goto cleanup;
    }

    ret = parse_response(raw_buf, out_buf, out_buf_len);

cleanup:
    free(auth_hdr);
    free(raw_buf);
    free(req_body);

    elapsed = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "llm_chat completed in %ums (status: %s)", elapsed, esp_err_to_name(ret));
    return ret;
}
