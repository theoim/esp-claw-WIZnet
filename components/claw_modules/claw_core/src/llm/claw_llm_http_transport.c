/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/claw_llm_http_transport.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "llm_http";

#define CLAW_LLM_HTTP_RB_INITIAL_CAP 4096

/* esp_http_client's timeout_ms is a per-I/O-op (select()) timeout, not a
 * total-request deadline — each successful read/write resets it, so a
 * healthy connection streaming a slow completion is unaffected. It only
 * fires when a single read or write call stalls. When an SPI proxy escape
 * hatch is registered, cap it well below the default (CLAW_LLM_DEFAULT_TIMEOUT_MS
 * in claw_llm_runtime.c, 25s): a stuck WiFi socket should fail fast into the
 * already-working proxy path rather than sit for 25s+ first. Observed cost of
 * the current 25s default: docs/DEVLOG.md S-track 14th entry (req=2, ~40s
 * total — ~29s stuck esp_tls_conn_write + ~11s proxy round trip). With this
 * cap the same case should land near 8s + ~11s ≈ 19s. Deployments without a
 * proxy registered (s_spi_proxy_fn == NULL) are unaffected. */
#define CLAW_LLM_HTTP_PROXY_FALLBACK_TIMEOUT_MS 8000u

static claw_llm_spi_proxy_fn_t s_spi_proxy_fn   = NULL;
static volatile bool            g_use_spi_proxy  = false;
/* Serialises concurrent WiFi-TLS LLM calls (claw_core + async-memory-extract).
 * Two simultaneous TLS handshakes exhaust the mbedtls ECC heap and crash with
 * LoadProhibited inside mbedtls_mpi_core_cond_assign. Initialised in
 * claw_llm_http_set_spi_proxy(), which is always called from app_main before
 * any LLM request can arrive. */
static SemaphoreHandle_t        s_llm_http_mutex = NULL;

void claw_llm_http_set_spi_proxy(claw_llm_spi_proxy_fn_t fn)
{
    s_spi_proxy_fn = fn;
    if (!s_llm_http_mutex) {
        s_llm_http_mutex = xSemaphoreCreateMutex();
    }
}
void claw_llm_http_set_proxy_mode(bool enabled)
{
    g_use_spi_proxy = enabled && (s_spi_proxy_fn != NULL);
    if (!s_llm_http_mutex) {
        s_llm_http_mutex = xSemaphoreCreateMutex();
    }
}
bool claw_llm_http_get_proxy_mode(void)         { return g_use_spi_proxy; }

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buffer_t;

typedef struct {
    response_buffer_t *buffer;
    volatile bool *abort_flag;
} http_request_context_t;

static inline bool abort_requested(const http_request_context_t *ctx)
{
    return ctx && ctx->abort_flag && *ctx->abort_flag;
}

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

static char *sanitize_utf8_body_copy(const char *body)
{
    size_t src = 0;
    size_t dst = 0;
    size_t len = 0;
    char *sanitized = NULL;

    if (!body) {
        return NULL;
    }

    len = strlen(body);
    /* Prefer PSRAM for the (potentially 20 KB+) sanitized copy so repeated
     * alloc/free cycles don't fragment internal RAM and starve cJSON_Parse
     * during context building on subsequent requests. */
    sanitized = heap_caps_calloc(1, len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sanitized) {
        sanitized = calloc(1, len + 1);
    }
    if (!sanitized) {
        return NULL;
    }

    while (body[src]) {
        unsigned char c = (unsigned char)body[src];

        if (c < 0x80) {
            sanitized[dst++] = body[src++];
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (body[src + 1] && ((unsigned char)body[src + 1] & 0xC0) == 0x80) {
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
            } else {
                sanitized[dst++] = ' ';
                src++;
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (body[src + 1] && body[src + 2] &&
                    ((unsigned char)body[src + 1] & 0xC0) == 0x80 &&
                    ((unsigned char)body[src + 2] & 0xC0) == 0x80) {
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
            } else {
                sanitized[dst++] = ' ';
                src++;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (body[src + 1] && body[src + 2] && body[src + 3] &&
                    ((unsigned char)body[src + 1] & 0xC0) == 0x80 &&
                    ((unsigned char)body[src + 2] & 0xC0) == 0x80 &&
                    ((unsigned char)body[src + 3] & 0xC0) == 0x80) {
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
                sanitized[dst++] = body[src++];
            } else {
                sanitized[dst++] = ' ';
                src++;
            }
        } else {
            sanitized[dst++] = ' ';
            src++;
        }
    }

    sanitized[dst] = '\0';
    return sanitized;
}

static esp_err_t response_buffer_init(response_buffer_t *buffer)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer->data = calloc(1, CLAW_LLM_HTTP_RB_INITIAL_CAP);
    if (!buffer->data) {
        return ESP_ERR_NO_MEM;
    }

    buffer->cap = CLAW_LLM_HTTP_RB_INITIAL_CAP;
    buffer->len = 0;
    return ESP_OK;
}

static esp_err_t response_buffer_append(response_buffer_t *buffer, const char *data, size_t len)
{
    char *grown;
    size_t cap;

    if (!buffer || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    cap = buffer->cap;
    while (buffer->len + len + 1 > cap) {
        cap *= 2;
    }

    if (cap != buffer->cap) {
        grown = realloc(buffer->data, cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        buffer->data = grown;
        buffer->cap = cap;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static void response_buffer_free(response_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }

    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_request_context_t *ctx = (http_request_context_t *)evt->user_data;

    if (abort_requested(ctx)) {
        return ESP_FAIL;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return response_buffer_append(ctx->buffer, (const char *)evt->data, evt->data_len);
    }

    return ESP_OK;
}

static char *build_auth_header_value(const char *auth_type, const char *api_key)
{
    const char *kind = auth_type ? auth_type : "bearer";

    if (!api_key || !api_key[0]) {
        return NULL;
    }
    if (strcmp(kind, "none") == 0) {
        return NULL;
    }
    if (strcmp(kind, "api-key") == 0) {
        return strdup(api_key);
    }

    return dup_printf("Bearer %s", api_key);
}

static const char *auth_header_name(const char *auth_type)
{
    if (auth_type && strcmp(auth_type, "api-key") == 0) {
        return "X-API-Key";
    }
    return "Authorization";
}

static char *parse_error_message_body(const char *body, int status)
{
    cJSON *root;
    cJSON *error;
    cJSON *message;
    char *fallback;

    if (!body || !body[0]) {
        return dup_printf("HTTP %d", status);
    }

    root = cJSON_Parse(body);
    if (!root) {
        return dup_printf("HTTP %d: %.160s", status, body);
    }

    error = cJSON_GetObjectItem(root, "error");
    if (error && cJSON_IsObject(error)) {
        message = cJSON_GetObjectItem(error, "message");
        if (message && cJSON_IsString(message) && message->valuestring[0]) {
            fallback = dup_printf("HTTP %d: %s", status, message->valuestring);
            cJSON_Delete(root);
            return fallback;
        }
    }

    message = cJSON_GetObjectItem(root, "message");
    if (message && cJSON_IsString(message) && message->valuestring[0]) {
        fallback = dup_printf("HTTP %d: %s", status, message->valuestring);
        cJSON_Delete(root);
        return fallback;
    }

    cJSON_Delete(root);
    return dup_printf("HTTP %d: %.160s", status, body);
}

esp_err_t claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
                                  claw_llm_http_response_t *out_response,
                                  char **out_error_message)
{
    response_buffer_t buffer = {0};
    http_request_context_t request_ctx = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char *auth_header_value = NULL;
    char *sanitized_body = NULL;
    int status_code = 0;
    esp_err_t err;

    if (out_response) {
        memset(out_response, 0, sizeof(*out_response));
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!request || !request->url || !request->body || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    sanitized_body = sanitize_utf8_body_copy(request->body);
    if (!sanitized_body) {
        *out_error_message = dup_printf("Out of memory sanitizing HTTP request body");
        ESP_LOGE(TAG, "OOM sanitizing HTTP request body");
        return ESP_ERR_NO_MEM;
    }

    /* ── SPI proxy path ───────────────────────────────────────────────
     * If proxy is enabled, forward request to Pico over SPI instead of
     * sending directly via WiFi. On proxy failure, fall through to the
     * direct HTTP path and clear the proxy flag.
     */
    if (g_use_spi_proxy && s_spi_proxy_fn) {
        char *proxy_auth = build_auth_header_value(request->auth_type, request->api_key);
        char *proxy_body = NULL;
        int   proxy_status = 0;
        esp_err_t perr = s_spi_proxy_fn(request->url, proxy_auth,
                                         sanitized_body, &proxy_body, &proxy_status);
        free(proxy_auth);
        if (perr == ESP_OK && proxy_body) {
            if (proxy_status == 200) {
                out_response->status_code = proxy_status;
                out_response->body = proxy_body;
                free(sanitized_body);
                return ESP_OK;
            }
            /* non-200 from proxy: treat as LLM API error */
            err = ESP_FAIL;
            *out_error_message = parse_error_message_body(proxy_body, proxy_status);
            ESP_LOGE(TAG, "SPI proxy LLM error %d: %s",
                     proxy_status, *out_error_message ? *out_error_message : "(null)");
            free(proxy_body);
            free(sanitized_body);
            return err;
        }
        /* proxy transport error → fall back to direct WiFi.
         * ESP_ERR_INVALID_STATE means proxy was busy (concurrent call):
         * keep g_use_spi_proxy so the next request retries via proxy. */
        free(proxy_body);
        if (perr != ESP_ERR_INVALID_STATE) {
            g_use_spi_proxy = false;
            ESP_LOGW(TAG, "SPI proxy failed (%s), falling back to direct HTTP",
                     esp_err_to_name(perr));
        } else {
            ESP_LOGW(TAG, "SPI proxy busy, using direct HTTP for this request");
        }
    }

    /* Acquire the WiFi-TLS slot before opening any TLS connection.
     * Prevents concurrent claw_core + async-memory-extract TLS handshakes
     * from exhausting the mbedtls ECC heap and causing LoadProhibited crash. */
    if (s_llm_http_mutex) {
        xSemaphoreTake(s_llm_http_mutex, portMAX_DELAY);
    }

    err = response_buffer_init(&buffer);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("Out of memory allocating HTTP buffer");
        ESP_LOGE(TAG, "OOM allocating HTTP response buffer");
        goto cleanup;
    }

    request_ctx.buffer = &buffer;
    request_ctx.abort_flag = request->abort_flag;
    config.url = request->url;
    config.event_handler = http_event_handler;
    config.user_data = &request_ctx;
    config.timeout_ms = request->timeout_ms;
    if (s_spi_proxy_fn && config.timeout_ms > CLAW_LLM_HTTP_PROXY_FALLBACK_TIMEOUT_MS) {
        config.timeout_ms = CLAW_LLM_HTTP_PROXY_FALLBACK_TIMEOUT_MS;
    }
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = NULL;  /* skip cert bundle; encrypted but no CA verify */

    client = esp_http_client_init(&config);
    if (!client) {
        *out_error_message = dup_printf("Failed to create HTTP client");
        ESP_LOGE(TAG, "Failed to create HTTP client for %s", request->url);
        err = ESP_FAIL;
        goto cleanup;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    auth_header_value = build_auth_header_value(request->auth_type, request->api_key);
    if (auth_header_value) {
        esp_http_client_set_header(client, auth_header_name(request->auth_type), auth_header_value);
    }
    if (request->headers && request->header_count > 0) {
        size_t i;

        for (i = 0; i < request->header_count; i++) {
            const claw_llm_http_header_t *header = &request->headers[i];

            if (!header->name || !header->name[0] || !header->value) {
                continue;
            }
            esp_http_client_set_header(client, header->name, header->value);
        }
    }
    esp_http_client_set_post_field(client, sanitized_body, (int)strlen(sanitized_body));

    ESP_LOGD(TAG, "POST %s", request->url);
    err = esp_http_client_perform(client);
    /* The jittered retry below exists for a specific race: two tasks (claw_core
     * + async-memory-extract) issuing concurrent lwip DNS queries for the same
     * hostname can both get EAI_AGAIN. That race is impossible whenever
     * s_spi_proxy_fn is registered, because s_llm_http_mutex (taken above,
     * near the top of this function) already serialises every call into this
     * function in that build. So when a proxy escape hatch exists, skip this
     * retry and fall straight through to it below: a second WiFi attempt here
     * costs up to ~18s (jitter + a possible DNS-resolution stall — see
     * docs/DEVLOG.md P-track 17th/18th entries) for a connection that just
     * failed once, when the proxy path reliably lands in ~10s. Deployments
     * without a proxy (s_spi_proxy_fn == NULL, mutex not created) keep the
     * original retry — the EAI_AGAIN race is still possible there. */
    if (err == ESP_ERR_HTTP_CONNECT && !abort_requested(&request_ctx) && !s_spi_proxy_fn) {
        uint32_t jitter_ms = 1000 + (esp_random() % 3000);
        ESP_LOGW(TAG, "HTTP connect failed, retry in %lums: %s", (unsigned long)jitter_ms, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(jitter_ms));
        esp_http_client_close(client);
        err = esp_http_client_perform(client);
    }
    if (err != ESP_OK) {
        if (abort_requested(&request_ctx)) {
            *out_error_message = dup_printf("HTTP request aborted by caller");
            ESP_LOGW(TAG, "HTTP perform aborted: %s", esp_err_to_name(err));
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));

        /* Inline SPI proxy retry: tear down the broken WiFi client and
         * immediately forward the same request to Pico over SPI.
         * This lets Pico's relay (still waiting for LLM_RESP) get a valid
         * response without the 35-second timeout. */
        if (s_spi_proxy_fn) {
            g_use_spi_proxy = true;
            ESP_LOGW(TAG, "HTTP failed, retrying via SPI proxy inline");

            free(auth_header_value);
            auth_header_value = NULL;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
            response_buffer_free(&buffer);

            char *proxy_auth = build_auth_header_value(request->auth_type, request->api_key);
            char *proxy_body = NULL;
            int   proxy_status = 0;
            esp_err_t perr = s_spi_proxy_fn(request->url, proxy_auth,
                                             sanitized_body, &proxy_body, &proxy_status);
            free(proxy_auth);
            if (perr == ESP_OK && proxy_body) {
                if (proxy_status == 200) {
                    out_response->status_code = proxy_status;
                    out_response->body = proxy_body;
                    free(sanitized_body);
                    if (s_llm_http_mutex) { xSemaphoreGive(s_llm_http_mutex); }
                    return ESP_OK;
                }
                /* Groq returned an HTTP error (4xx/5xx) — proxy is reachable.
                 * Keep g_use_spi_proxy: WiFi is still down. */
                err = ESP_FAIL;
                *out_error_message = parse_error_message_body(proxy_body, proxy_status);
                ESP_LOGE(TAG, "SPI proxy LLM error %d: %s",
                         proxy_status, *out_error_message ? *out_error_message : "(null)");
                free(proxy_body);
                free(sanitized_body);
                if (s_llm_http_mutex) { xSemaphoreGive(s_llm_http_mutex); }
                return err;
            }
            free(proxy_body);
            /* Proxy call failed (timeout / busy / CRC error → Pico sent 500 with no body).
             * Do NOT clear g_use_spi_proxy — WiFi is still down, proxy is still the path. */
            *out_error_message = dup_printf("HTTP and SPI proxy both failed: %s",
                                            esp_err_to_name(perr));
            ESP_LOGE(TAG, "inline proxy also failed (%s)", esp_err_to_name(perr));
            free(sanitized_body);
            if (s_llm_http_mutex) { xSemaphoreGive(s_llm_http_mutex); }
            return ESP_FAIL;
        }

        *out_error_message = dup_printf("HTTP request failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (abort_requested(&request_ctx)) {
        *out_error_message = dup_printf("HTTP request aborted by caller");
        ESP_LOGW(TAG, "HTTP perform completed after caller abort");
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "HTTP status=%d", status_code);
    if (status_code != 200) {
        err = ESP_FAIL;
        *out_error_message = parse_error_message_body(buffer.data, status_code);
        ESP_LOGE(TAG, "LLM error: %s", *out_error_message ? *out_error_message : "(null)");
        goto cleanup;
    }

    out_response->status_code = status_code;
    out_response->body = buffer.data;
    buffer.data = NULL;
    err = ESP_OK;

cleanup:
    free(auth_header_value);
    free(sanitized_body);
    if (client) {
        /* Close before cleanup: prevents LoadProhibited crash in
         * esp_transport_destroy_foundation_transport when TLS state was
         * corrupted by an abrupt WiFi disconnect mid-connection. */
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    response_buffer_free(&buffer);
    if (s_llm_http_mutex) { xSemaphoreGive(s_llm_http_mutex); }
    return err;
}

void claw_llm_http_response_free(claw_llm_http_response_t *response)
{
    if (!response) {
        return;
    }

    free(response->body);
    memset(response, 0, sizeof(*response));
}
