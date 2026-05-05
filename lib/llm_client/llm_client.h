#pragma once

#include "esp_err.h"
#include <stddef.h>

#define LLM_MAX_PROMPT_LEN   1024
#define LLM_MAX_RESPONSE_LEN 8192

/**
 * Send a user message to OpenRouter and write the reply into out_buf.
 */
esp_err_t llm_chat(const char *api_key,
                   const char *model,
                   const char *user_prompt,
                   char       *out_buf,
                   size_t      out_buf_len);