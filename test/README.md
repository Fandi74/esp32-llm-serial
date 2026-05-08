# ESP32-S3 LLM Serial — Test Documentation

## Overview

This document provides comprehensive test procedures for the ESP32-S3 LLM Serial project, targeting the **ESP32-S3-DevKitM-1** board. The project enables serial-prompt interaction with LLMs via the OpenRouter API.

**Key Components:**
- [`src/main.c`](src/main.c) — REPL task with serial I/O
- [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c) — HTTP client + JSON parser
- [`lib/llm_client/llm_client.h`](lib/llm_client/llm_client.h) — Public API (`llm_chat()`)
- [`lib/wifi_manager/wifi_manager.c`](lib/wifi_manager/wifi_manager.c) — Wi-Fi station connect
- [`include/api.h`](include/api.h) — Credentials (copy from [`include/api.h.example`](include/api.h.example))

**Buffer Constants:**
| Constant | Value | Purpose |
|----------|-------|---------|
| `LLM_MAX_PROMPT_LEN` | 1024 | Max user input length |
| `LLM_MAX_RESPONSE_LEN` | 8192 | Max assistant reply length |
| `HTTP_TIMEOUT_MS` | 30000 | HTTP transport timeout |
| `LLM_CHAT_TIMEOUT_MS` | 60000 | Overall operation timeout |

---

## Prerequisites

### Hardware
- ESP32-S3-DevKitM-1 board
- USB cable (data-capable)
- Serial terminal (e.g., `idf.py monitor`, PuTTY, screen)

### Software Setup
1. **Copy credentials template:**
   ```bash
   cp include/api.h.example include/api.h
   ```

2. **Edit [`include/api.h`](include/api.h) with valid values:**
   ```c
   #define WIFI_SSID      "your_actual_ssid"
   #define WIFI_PASSWORD  "your_actual_password"
   #define OPENROUTER_API_KEY  "sk-or-v1-valid_key_here"
   #define OPENROUTER_MODEL    "meta-llama/llama-3.1-8b-instruct:free"
   ```

3. **Build and flash:**
   ```bash
   idf.py build flash monitor
   ```
   Or with PlatformIO:
   ```bash
   pio run -t upload && pio device monitor
   ```

### Serial Monitor Settings
- **Baud rate:** 115200
- **Line ending:** LF or CRLF (the REPL strips both)
- **Flow control:** None

---

## Critical-Path Testing (Must Pass)

These tests **must pass** before any release. They validate core functionality.

### Test 1: Wi-Fi Connection

**Objective:** Verify the device connects to the configured Wi-Fi SSID and obtains an IP address.

**Prerequisites:**
- Valid `WIFI_SSID` and `WIFI_PASSWORD` in [`include/api.h`](include/api.h)
- Wi-Fi network is operational and in range

**Steps:**
1. Flash the firmware
2. Open serial monitor
3. Observe boot log

**Expected Results:**
```
I (xxx) wifi: new management frame while waiting for action frame
I (xxx) wifi: AP associated, starting DHCP client
I (xxx) esp_netif_handlers: sta ip: 192.168.x.x, mask: 255.255.255.0, gw: 192.168.x.1
I (xxx) main: Wi-Fi connected successfully
```

**Success Criteria:**
- `[`src/main.c`](src/main.c:92)` `wifi_manager_connect()` returns `ESP_OK`
- IP address is assigned (not 0.0.0.0)
- No `ESP_LOGE` messages related to Wi-Fi

**Serial Output to Observe:**
```
I (xxx) wifi_manager: Connecting to SSID: your_ssid
I (xxx) wifi: ...
I (xxx) esp_netif_handlers: sta ip: 192.168.1.100, mask: 255.255.255.0, gw: 192.168.1.1
```

---

### Test 2: Successful LLM Prompt

**Objective:** Send a prompt and receive a valid assistant reply from OpenRouter API.

**Prerequisites:**
- Wi-Fi connected (Test 1 passed)
- Valid `OPENROUTER_API_KEY` in [`include/api.h`](include/api.h)
- OpenRouter account has available credits

**Steps:**
1. Wait for the REPL banner:
   ```
   ========================================
     esp32-llm-serial
     Model : meta-llama/llama-3.1-8b-instruct:free
     Type a message and press Enter.
     Type 'exit' to quit.
   ========================================
   ```
2. At the `You: ` prompt, type: `Hello, what is 2+2?`
3. Press Enter

**Expected Results:**
```
You: Hello, what is 2+2?
Assistant: thinking...
(...)
Assistant: 2+2 equals 4.
```

**Success Criteria:**
- `llm_chat()` returns `ESP_OK`
- Reply is non-empty and contains coherent text
- No `ESP_LOGE` messages in the serial output
- HTTP status 200 logged: `I (xxx) llm_client: POST → OpenRouter [model] (timeout 30000ms)`

**Serial Output to Observe:**
```
I (xxx) llm_client: POST → OpenRouter [meta-llama/llama-3.1-8b-instruct:free] (timeout 30000ms)
I (xxx) llm_client: llm_chat completed in xxxms (status: ESP_OK)
```

---

### Test 3: Forced Failure (Bad API Key)

**Objective:** Confirm error handling when an invalid API key is used.

**Prerequisites:**
- Wi-Fi connected
- Ability to modify [`include/api.h`](include/api.h)

**Steps:**
1. Edit [`include/api.h`](include/api.h) and set:
   ```c
   #define OPENROUTER_API_KEY  "sk-or-v1-INVALID_KEY"
   ```
2. Rebuild and flash:
   ```bash
   idf.py build flash monitor
   ```
3. At the `You: ` prompt, type: `Test message`
4. Press Enter

**Expected Results:**
```
You: Test message
Assistant: thinking...
(...)
[Error: ESP_FAIL]
```

**Success Criteria:**
- `llm_chat()` returns `ESP_FAIL`
- HTTP 401 error is logged:
  ```
  E (xxx) llm_client: HTTP 401: {"error":{"message":"Invalid API key","code":401}}
  E (xxx) llm_client: Hint: Check API key validity
  ```
- No crash or reboot occurs

**Serial Output to Observe:**
```
E (xxx) llm_client: HTTP 401: {"error":{"message":"Invalid Authentication","code":401}}
E (xxx) llm_client: Hint: Check API key validity
E (xxx) llm_client: llm_chat completed in xxxms (status: ESP_FAIL)
```

**Restore Valid Key:**
After test, restore valid API key and reflash.

---

## Thorough Testing (Edge Cases)

These tests validate robustness and error handling.

### Test 4: Response Truncation (Exceeds LLM_MAX_RESPONSE_LEN)

**Objective:** Verify handling when the API response exceeds `LLM_MAX_RESPONSE_LEN` (8192 bytes).

**Prerequisites:**
- Valid API key
- Model that can generate long responses (e.g., `meta-llama/llama-3.1-8b-instruct:free`)

**Steps:**
1. Edit [`include/api.h`](include/api.h) to use a model that generates long output
2. Send a prompt that elicits a long response:
   ```
   You: Write a 2000-word essay about the history of microprocessors
   ```
3. Press Enter

**Expected Results:**
- If response > `raw_len` (3 × `out_buf_len` = 24576 bytes), overflow is logged:
  ```
  W (xxx) llm_client: Response buffer overflow — truncating (written=24575, incoming=1024, buf_len=24576)
  ```
- `llm_chat()` may return `ESP_FAIL` if overflow occurs
- Reply in REPL is truncated but does not crash

**Success Criteria:**
- No buffer overflow/corruption
- `ctx.overflow` flag is set in [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c:38)
- System remains stable

---

### Test 5: Malformed/Changed API Response

**Objective:** Test handling of unexpected API response formats.

**Prerequisites:**
- Ability to modify [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c) for testing (or use a mock server)

**Steps (Simulated via Code Modification):**
1. Temporarily modify [`parse_response()`](lib/llm_client/llm_client.c:85) to inject malformed JSON:
   ```c
   // In llm_chat(), before parse_response() call:
   strcpy(raw_buf, "{\"invalid_structure\": true}");
   ```
2. Rebuild and flash
3. Send any prompt

**Expected Results:**
```
E (xxx) llm_client: Invalid or missing 'choices' array in response
E (xxx) llm_client: llm_chat completed in xxxms (status: ESP_FAIL)
```

**Success Criteria:**
- `parse_response()` detects missing `choices` array
- Returns `ESP_FAIL`
- No crash or undefined behavior

**Note:** Remove test modification after validation.

---

### Test 6: Empty Response from API

**Objective:** Verify handling when the API returns HTTP 200 but an empty body.

**Prerequisites:**
- Ability to simulate empty response (modify code temporarily)

**Steps (Simulated):**
1. Temporarily modify [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c) to force empty body:
   ```c
   // In llm_chat(), after HTTP perform:
   ctx.written = 0;
   raw_buf[0] = '\0';
   ```
2. Rebuild and flash
3. Send a prompt

**Expected Results:**
```
E (xxx) llm_client: Empty response body from API (HTTP 200 but no data)
E (xxx) llm_client: llm_chat completed in xxxms (status: ESP_FAIL)
```

**Success Criteria:**
- Detected by [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c:270) `ctx.written == 0` check
- Returns `ESP_FAIL`
- Clear error message logged

---

### Test 7: HTTP Errors (401, 429, 500)

**Objective:** Validate handling of various HTTP error status codes.

#### Test 7a: HTTP 401 (Unauthorized)
Already covered in Critical-Path Test 3.

#### Test 7b: HTTP 429 (Rate Limited)
**Steps:**
1. Send multiple rapid prompts (may require script or fast typing)
2. Observe rate limit handling

**Expected Results:**
```
E (xxx) llm_client: HTTP 429: {"error":{"message":"Rate limit exceeded","code":429}}
E (xxx) llm_client: Hint: Rate limited — reduce request frequency
```

#### Test 7c: HTTP 500 (Server Error)
**Steps:**
1. Use a model that may trigger server errors, or temporarily block API responses
2. Send a prompt

**Expected Results:**
```
E (xxx) llm_client: HTTP 500: <html>...</html>
E (xxx) llm_client: Hint: Server internal error — retry later
```

**Success Criteria:**
- Status code checked at [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c:250)
- Appropriate hint logged for each status code
- Returns `ESP_FAIL`

---

### Test 8: Network Timeout Scenarios

**Objective:** Test behavior when the network times out or becomes unavailable.

**Prerequisites:**
- Ability to disconnect Wi-Fi during operation

**Steps:**
1. Connect to Wi-Fi and start REPL
2. Send a prompt: `You: Quick test`
3. **Immediately** disconnect Wi-Fi (turn off router or use `wifi_manager_disconnect()`)
4. Observe behavior

**Expected Results:**
- HTTP transport error logged:
  ```
  E (xxx) llm_client: HTTP transport error: ESP_ERR_HTTP_CONNECTION_FAILED
  ```
  Or timeout:
  ```
  E (xxx) llm_client: HTTP transport error: ESP_ERR_HTTP_TIMED_OUT
  ```
- `llm_chat()` returns error
- REPL remains active (does not crash)

**Success Criteria:**
- `esp_http_client_perform()` returns error at [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c:232)
- Cleanup performed (no memory leaks)
- User sees `[Error: ESP_ERR_HTTP_TIMED_OUT]` or similar

---

### Test 9: Invalid JSON in Response

**Objective:** Test parsing of non-JSON or malformed JSON responses.

**Prerequisites:**
- Ability to inject invalid JSON (code modification)

**Steps (Simulated):**
1. Temporarily modify response before parsing:
   ```c
   strcpy(raw_buf, "This is not JSON at all!");
   ```
2. Rebuild and flash
3. Send a prompt

**Expected Results:**
```
E (xxx) llm_client: JSON parse failed — invalid JSON: This is not JSON at all!
E (xxx) llm_client: llm_chat completed in xxxms (status: ESP_FAIL)
```

**Success Criteria:**
- `cJSON_Parse()` returns NULL at [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c:93)
- Error logged with first part of invalid JSON
- Returns `ESP_FAIL`

---

### Test 10: Buffer Boundary Conditions

**Objective:** Test prompt and response at maximum buffer lengths.

#### Test 10a: Maximum-Length Prompt
**Steps:**
1. Send a prompt that is exactly 1023 characters (leaving room for null terminator):
   ```
   You: [1023-char string]
   ```
   (Can generate with: `python -c "print('a'*1023)"` and paste)

**Expected Results:**
- Prompt accepted (fgets limits to `LLM_MAX_PROMPT_LEN`)
- `llm_chat()` processes normally

#### Test 10b: Prompt Exceeding Buffer
**Steps:**
1. Try to paste a prompt longer than 1023 characters

**Expected Results:**
- `fgets()` truncates to 1023 characters + null terminator
- No buffer overflow

**Success Criteria:**
- `LLM_MAX_PROMPT_LEN` respected in [`src/main.c`](src/main.c:45)
- No stack corruption

---

## Serial Console Commands/Examples

### Starting the REPL
After boot, the serial console shows:
```
========================================
  esp32-llm-serial
  Model : meta-llama/llama-3.1-8b-instruct:free
  Type a message and press Enter.
  Type 'exit' to quit.
========================================

You: 
```

### Example Prompts

**Simple question:**
```
You: What is the capital of France?
Assistant: thinking...
Assistant: The capital of France is Paris.
```

**Code generation:**
```
You: Write a C function to reverse a string
Assistant: thinking...
Assistant: Here's a C function to reverse a string:

```c
void reverse_string(char *str) {
    int len = strlen(str);
    for (int i = 0; i < len / 2; i++) {
        char temp = str[i];
        str[i] = str[len - i - 1];
        str[len - i - 1] = temp;
    }
}
```
```

**Exit the REPL:**
```
You: exit
Goodbye!
```

---

## Error Path Testing Summary

| Error Condition | How to Trigger | Expected Error | Log Keyword |
|----------------|----------------|----------------|-------------|
| Bad API key | Set invalid key in [`include/api.h`](include/api.h) | `ESP_FAIL` | `HTTP 401` |
| Wi-Fi disconnected | Turn off router during request | `ESP_ERR_HTTP_*` | `transport error` |
| Empty response | Simulate in code | `ESP_FAIL` | `Empty response body` |
| Invalid JSON | Simulate in code | `ESP_FAIL` | `JSON parse failed` |
| HTTP 429 (rate limit) | Send rapid requests | `ESP_FAIL` | `HTTP 429` |
| HTTP 500 (server error) | Use failing model | `ESP_FAIL` | `HTTP 500` |
| Buffer overflow | Long response | `ESP_FAIL` | `buffer overflow` |
| Invalid argument | NULL pointer (simulate) | `ESP_ERR_INVALID_ARG` | `Invalid argument` |

---

## What to Observe in Serial Monitor

### Normal Operation
- `I (xxx)` messages: Informational (success)
- `llm_chat completed in xxxms (status: ESP_OK)`

### Warnings
- `W (xxx)` messages: Warnings (e.g., truncation)
- Example: `Response buffer overflow — truncating`

### Errors
- `E (xxx)` messages: Errors (failures)
- Example: `HTTP 401: ...`, `JSON parse failed`, `HTTP transport error`

### Memory Leaks
- Enable heap tracing in `idf.py menuconfig`:
  - `Component config → Heap memory debugging → Enable heap tracing`
- Watch for leaks in `llm_chat()` (should free `auth_hdr`, `raw_buf`, `req_body`)

---

## Test Execution Checklist

### Critical-Path (Must Pass)
- [ ] Test 1: Wi-Fi connects successfully
- [ ] Test 2: Valid prompt returns assistant reply
- [ ] Test 3: Bad API key returns error (no crash)

### Thorough Testing
- [ ] Test 4: Response truncation handled gracefully
- [ ] Test 5: Malformed API response detected
- [ ] Test 6: Empty response handled
- [ ] Test 7: HTTP errors (401, 429, 500) handled
- [ ] Test 8: Network timeout handled
- [ ] Test 9: Invalid JSON detected
- [ ] Test 10: Buffer boundaries respected

### Regression Tests (After Changes)
- [ ] Re-run Critical-Path tests after any code change
- [ ] Check memory usage: `idf.py monitor` → `heap_caps_get_free_size()`
- [ ] Verify no new compiler warnings: `idf.py build 2>&1 | grep warning`

---

## Build & Flash Commands

### ESP-IDF (Official)
```bash
# Set target
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor
idf.py flash monitor

# Just monitor (after flashing)
idf.py monitor
```

### PlatformIO
```bash
# Build
pio run

# Upload
pio run -t upload

# Monitor
pio device monitor
```

### Monitor Exit
- ESP-IDF: `Ctrl+]`
- PlatformIO: `Ctrl+C` then `Ctrl+]` or type `exit`

---

## Notes

1. **API Key Security:** Never commit [`include/api.h`](include/api.h) with real credentials. Use [`include/api.h.example`](include/api.h.example) as template.

2. **Model Selection:** Free models on OpenRouter may have rate limits. Check [openrouter.ai/models](https://openrouter.ai/models) for current free options.

3. **Timeout Values:** 
   - `HTTP_TIMEOUT_MS = 30000` (30s) — transport timeout
   - `LLM_CHAT_TIMEOUT_MS = 60000` (60s) — overall operation timeout
   - Adjust in [`lib/llm_client/llm_client.c`](lib/llm_client/llm_client.c) if needed

4. **Stack Size:** REPL task uses 6144 bytes stack ([`src/main.c`](src/main.c:98)). Monitor for overflow:
   ```
   Guru Meditation Error: Core 0 panic'ed (Stack overflow)
   ```
   Increase if adding deep call chains.

5. **Static Allocation:** Project uses static allocation (`configASSERT`, `ESP_ERROR_CHECK`). No dynamic allocation in hot paths except `llm_chat()` buffers (freed before return).

---

**Document Version:** 1.0  
**Last Updated:** 2026-05-08  
**Target Board:** ESP32-S3-DevKitM-1
