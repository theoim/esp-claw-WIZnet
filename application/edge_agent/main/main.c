/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_fs.h"
#include "claw_paths.h"
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"
#if CONFIG_NETWORK_BACKEND_WIRED
#include "esp_netif.h"
#include "esp_event.h"
#endif
#if CONFIG_SPI_CAM_BRIDGE_ENABLE
#include "spi_wiz.h"
#include "driver/gpio.h"
#include "claw_paths.h"
#include "claw_agent_mgr.h"
#include "cJSON.h"
#include "llm/claw_llm_http_transport.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif
#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
#include "camera_hal.h"
#include "esp_jpeg_enc.h"
#include "esp_jpeg_common.h"
#if CONFIG_PERSON_DETECT_ENABLE
#include "person_detect.h"
#endif
#endif
#endif

#define APP_ENABLE_MEM_LOG        (0)

/* ── PIR motion sensor ────────────────────────────────────────────────
 * Change PIR_GPIO_PIN to match your wiring.
 * PIR OUT → ESP32 GPIO10 (active HIGH, 3.3V compatible).
 * PIR VCC → 3.3V or 5V depending on module; GND → GND.
 * Debounce: alerts sent at most once per PIR_DEBOUNCE_MS (30s). */
#define PIR_GPIO_PIN      44
#define PIR_DEBOUNCE_MS   30000

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;

#if CONFIG_SPI_CAM_BRIDGE_ENABLE
static spi_wiz_handle_t s_spi_wiz;

static volatile bool s_pong_received = false;

/* Real WiFi STA state, mirrored from on_wifi_state_changed(). Reported in the
 * SPI_CMD_ESP_STATUS heartbeat so the Pico guardian sees actual connectivity
 * instead of a hardcoded value. */
static volatile bool s_wifi_connected = false;

/* ── SPI HTTP proxy (ESP32 → Pico → Ethernet → Groq) ─────────────────
 * Used when WiFi is unavailable or failed. Pico forwards the HTTP POST
 * over Ethernet and returns the response via SPI.
 */
static SemaphoreHandle_t s_http_proxy_sem      = NULL;
static SemaphoreHandle_t s_spi_proxy_lock      = NULL;  /* serialise concurrent proxy calls */
static char             *s_http_proxy_resp_body = NULL;
static size_t            s_http_proxy_resp_cap  = 0;
static size_t            s_http_proxy_resp_len  = 0;
static int               s_http_proxy_resp_status = 0;

static esp_err_t spi_http_proxy_fn(const char *url, const char *auth_header,
                                    const char *body_json,
                                    char **out_body, int *out_status)
{
    if (!s_spi_wiz || !s_http_proxy_sem || !s_spi_proxy_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialise concurrent proxy calls: wait up to 30 s for any in-flight proxy
     * to complete before starting a new one.  The previous 0-timeout caused
     * the async claw_memory extraction task to fail immediately when the main
     * LLM request was already using the proxy, propagating back as claw_core
     * failure and triggering a spurious ok=false LLM_RESP to the Pico. */
    if (xSemaphoreTake(s_spi_proxy_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGW("spi_proxy", "proxy busy timeout (concurrent call)");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    size_t body_len = strlen(body_json);

    /* send metadata packet */
    char meta[768];
    int mlen = snprintf(meta, sizeof(meta),
                        "{\"url\":\"%s\",\"auth\":\"%s\",\"body_len\":%zu}",
                        url ? url : "",
                        auth_header ? auth_header : "",
                        body_len);
    if (mlen <= 0 || (size_t)mlen >= sizeof(meta)) {
        result = ESP_ERR_INVALID_SIZE;
        goto unlock;
    }

    /* Reset response buffer and drain stale semaphore BEFORE sending the
     * request. Pico may respond (e.g. 500 on CRC error) before BODY_END
     * arrives; if we reset and drain AFTER sending, we overwrite the valid
     * response and have to wait 20s for the next spi_status_task cycle to
     * unblock Pico's stuck spi_write_blocking call. */
    free(s_http_proxy_resp_body);
    s_http_proxy_resp_body   = NULL;
    s_http_proxy_resp_cap    = 0;
    s_http_proxy_resp_len    = 0;
    s_http_proxy_resp_status = 0;
    xSemaphoreTake(s_http_proxy_sem, 0);  /* drain stale from previous call */

    ESP_LOGI("spi_proxy", "HTTP_REQ url=%.60s body=%zu bytes", url ? url : "", body_len);
    result = spi_wiz_send(s_spi_wiz, SPI_CMD_HTTP_REQ,
                          (const uint8_t *)meta, (uint16_t)mlen);
    if (result != ESP_OK) { goto unlock; }
    vTaskDelay(pdMS_TO_TICKS(20));  /* Pico needs time to parse HTTP_REQ JSON + malloc body buf */

    /* send body in chunks */
    {
        const char *ptr = body_json;
        size_t remaining = body_len;
        while (remaining > 0) {
            uint16_t n = (uint16_t)(remaining > SPI_CLAW_MAX_CHUNK
                                     ? SPI_CLAW_MAX_CHUNK : remaining);
            result = spi_wiz_send(s_spi_wiz, SPI_CMD_HTTP_BODY,
                                  (const uint8_t *)ptr, n);
            if (result != ESP_OK) { goto unlock; }
            ptr += n;
            remaining -= n;
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    /* signal body complete */
    result = spi_wiz_send(s_spi_wiz, SPI_CMD_HTTP_BODY_END, NULL, 0);
    if (result != ESP_OK) { goto unlock; }

    /* wait for Pico to complete HTTP POST and return response (45s).
     * Pico TLS handshake + HTTP round-trip for large payloads (20KB+) can
     * exceed 20s; previous timeout caused spurious proxy-fail + LLM_RESP error. */
    if (xSemaphoreTake(s_http_proxy_sem, pdMS_TO_TICKS(45000)) != pdTRUE) {
        ESP_LOGE("spi_proxy", "response timeout");
        result = ESP_ERR_TIMEOUT;
        goto unlock;
    }

    *out_status = s_http_proxy_resp_status;
    *out_body   = s_http_proxy_resp_body;
    s_http_proxy_resp_body = NULL;  /* transfer ownership to caller */
    ESP_LOGI("spi_proxy", "response status=%d len=%zu",
             *out_status, *out_body ? strlen(*out_body) : 0);
    result = ESP_OK;

unlock:
    xSemaphoreGive(s_spi_proxy_lock);
    return result;
}

static TaskHandle_t s_pir_task_handle = NULL;

static void IRAM_ATTR pir_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_pir_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static void pir_send_task(void *arg)
{
    (void)arg;
    static const char pev[] = "{\"type\":\"pir\"}";
    int64_t last_us = -(int64_t)PIR_DEBOUNCE_MS * 1000;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (now - last_us < (int64_t)PIR_DEBOUNCE_MS * 1000) {
            continue;
        }
        last_us = now;
        ESP_LOGI(TAG, "PIR motion detected → SPI_CMD_EVENT");
        spi_wiz_send(s_spi_wiz, SPI_CMD_EVENT,
                     (const uint8_t *)pev, sizeof(pev) - 1);
    }
}

#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
#define CAPTURE_DEV_PATH    "/dev/video2"
#define CAPTURE_TASK_STACK  12288

/* Persistent camera: open once on first CAPTURE_REQ, stay open.
 * Avoids 30s re-settle on every request (MJPG settle is unreliable on OV3660 DVP). */
static bool     s_camera_opened = false;
static uint32_t s_cam_w         = 640;
static uint32_t s_cam_h         = 480;

static void s_capture_task(void *arg)
{
    esp_err_t  err        = ESP_OK;
    uint8_t   *frame      = NULL;
    size_t     frame_bytes = 0;
    uint8_t   *jpeg_out   = NULL;
    char       mime[16]   = "image/jpeg";

#if CONFIG_PERSON_DETECT_ENABLE
    person_detect_init();  /* no-op if already initialized */
#endif

    if (!s_camera_opened) {
        /* pre-open task가 stream settle (~30s) 중 mutex 점유 가능.
         * 20회 × 1500ms = 최대 30s 대기 → pre-open 포기 후 mutex 반환되면 성공.
         * Pico의 CAPTURE_REQ 타임아웃(90s) 이내에 완료. */
        camera_open_opts_t opts = { 0 };
        for (int _try = 0; _try < 20; _try++) {
            if (_try > 0) { vTaskDelay(pdMS_TO_TICKS(1500)); }
            err = camera_open(CAPTURE_DEV_PATH, &opts);
            if (err == ESP_OK) { break; }
            ESP_LOGW(TAG, "camera_open attempt %d failed: %s", _try + 1, esp_err_to_name(err));
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "camera_open: all attempts failed");
            goto send_end;
        }
        s_camera_opened = true;

        camera_stream_info_t si = { 0 };
        if (camera_get_stream_info(&si) == ESP_OK) {
            s_cam_w = si.width  ? si.width  : 640;
            s_cam_h = si.height ? si.height : 480;
            ESP_LOGI(TAG, "Camera: %"PRIu32"x%"PRIu32" fmt=%s",
                     s_cam_w, s_cam_h, si.pixel_format_str);
        }
    }

    /* Drain up to 3 stale frames from DVP triple-buffer before capturing.
     * Without this, back-to-back requests return an old buffered frame,
     * which can appear as a colour-inverted duplicate of the previous shot. */
    {
        uint8_t *stale       = NULL;
        size_t   stale_bytes = 0;
        for (int _f = 0; _f < 3; _f++) {
            if (camera_capture_frame(100, &stale, &stale_bytes, NULL) == ESP_OK && stale) {
                camera_release_frame(stale);
                stale = NULL;
            } else {
                break;
            }
        }
    }

    err = camera_capture_frame(5000, &frame, &frame_bytes, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_capture_frame: %s", esp_err_to_name(err));
        s_camera_opened = false;
        camera_close();
        goto send_end;
    }

#if CONFIG_PERSON_DETECT_ENABLE
    {
        int score = person_detect_run(frame, (int)s_cam_w, (int)s_cam_h);
        if (score < 0) {
            ESP_LOGW(TAG, "TFLite inference error, proceeding anyway");
        } else if (score < CONFIG_PERSON_DETECT_THRESHOLD) {
            ESP_LOGI(TAG, "No person detected (%d%% < %d%%), skip capture",
                     score, CONFIG_PERSON_DETECT_THRESHOLD);
            camera_release_frame(frame);
            frame = NULL;
            frame_bytes = 0;  /* force CHUNK_END size=0; Pico skips Vision/Telegram */
            goto send_end;
        } else {
            ESP_LOGI(TAG, "Person detected (%d%%), proceeding with JPEG+Vision", score);
        }
    }
#endif

    /* YUYV → JPEG via esp_new_jpeg block encoder.
     * Block-by-block copy avoids 600KB aligned alloc (internal SRAM too small). */
    {
        size_t yuyv_bytes = (size_t)s_cam_w * s_cam_h * 2;
        if (frame_bytes < yuyv_bytes) { yuyv_bytes = frame_bytes; }

        jpeg_enc_config_t enc_cfg = DEFAULT_JPEG_ENC_CONFIG();
        enc_cfg.width       = (int)s_cam_w;
        enc_cfg.height      = (int)s_cam_h;
        enc_cfg.quality     = 60;
        enc_cfg.task_enable = false;

        jpeg_enc_handle_t enc = NULL;
        jpeg_error_t jerr = jpeg_enc_open(&enc_cfg, &enc);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "jpeg_enc_open: %d", (int)jerr);
            camera_release_frame(frame); frame = NULL;
            goto send_end;
        }

        /* Block buffer: one MCU-row strip (e.g. 640×16×2 = 20480 bytes) */
        int block_size = jpeg_enc_get_block_size(enc);
        uint8_t *block_in = jpeg_calloc_align(block_size, 16);
        if (!block_in) {
            ESP_LOGE(TAG, "OOM jpeg block (%d)", block_size);
            jpeg_enc_close(enc);
            camera_release_frame(frame); frame = NULL;
            goto send_end;
        }

        /* Output: ~50-100KB for 640×480 q60. malloc uses PSRAM on this board. */
        int out_cap = 150 * 1024;
        jpeg_out = malloc(out_cap);
        if (!jpeg_out) {
            ESP_LOGE(TAG, "OOM jpeg output (%d)", out_cap);
            jpeg_free_align(block_in);
            jpeg_enc_close(enc);
            camera_release_frame(frame); frame = NULL;
            goto send_end;
        }

        size_t src_offset = 0;
        int enc_size = 0;
        bool enc_ok = false;
        while (src_offset < yuyv_bytes) {
            size_t avail = yuyv_bytes - src_offset;
            size_t to_copy = avail < (size_t)block_size ? avail : (size_t)block_size;
            memcpy(block_in, frame + src_offset, to_copy);
            src_offset += to_copy;
            enc_size = 0;
            jerr = jpeg_enc_process_with_block(enc, block_in, (int)to_copy,
                                               jpeg_out, out_cap, &enc_size);
            if (jerr == JPEG_ERR_OK) { enc_ok = true; break; }
            if (jerr < 0) {
                ESP_LOGE(TAG, "jpeg_enc_process_with_block: err=%d", (int)jerr);
                break;
            }
            /* jerr == block_size → continue */
        }

        jpeg_free_align(block_in);
        jpeg_enc_close(enc);
        camera_release_frame(frame); frame = NULL;

        if (!enc_ok || enc_size <= 0) {
            free(jpeg_out); jpeg_out = NULL;
            goto send_end;
        }

        frame_bytes = (size_t)enc_size;
        ESP_LOGI(TAG, "JPEG %"PRIu32"x%"PRIu32": YUYV=%zu → JPEG=%zu bytes, %u chunk(s)",
                 s_cam_w, s_cam_h, yuyv_bytes, frame_bytes,
                 (unsigned)((frame_bytes + SPI_CLAW_MAX_CHUNK - 1) / SPI_CLAW_MAX_CHUNK));

        const uint8_t *ptr = jpeg_out;
        size_t remaining = frame_bytes;
        while (remaining > 0) {
            uint16_t n = (uint16_t)(remaining > SPI_CLAW_MAX_CHUNK
                                    ? SPI_CLAW_MAX_CHUNK : remaining);
            if (spi_wiz_send(s_spi_wiz, SPI_CMD_CHUNK_DATA, ptr, n) != ESP_OK) {
                ESP_LOGE(TAG, "CHUNK_DATA send failed, aborting");
                frame_bytes = 0;
                break;
            }
            ptr += n;
            remaining -= n;
            /* 2ms inter-chunk gap: Pico callback (memcpy) + CRC check finishes
             * before next CS↓; prevents SSP FIFO overflow on back-to-back chunks. */
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        /* Extra guard before CHUNK_END: Pico needs to finish CRC check and
         * return to poll() before the next CS↓ (at 2MHz CHUNK_END ≈ 148μs). */
        if (frame_bytes > 0) { vTaskDelay(pdMS_TO_TICKS(20)); }

        free(jpeg_out); jpeg_out = NULL;
    }

send_end:
    if (frame)    { camera_release_frame(frame); }
    if (jpeg_out) { free(jpeg_out); }
    {
        char info[64];
        int  ilen = snprintf(info, sizeof(info),
                             "{\"mime\":\"%s\",\"size\":%zu}", mime, frame_bytes);
        spi_wiz_send(s_spi_wiz, SPI_CMD_CHUNK_END,
                     (const uint8_t *)info, (uint16_t)ilen);
        ESP_LOGI(TAG, "CHUNK_END sent (size=%zu)", frame_bytes);
    }
    vTaskDelete(NULL);
}
#endif /* CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT */

typedef struct {
    uint32_t request_id;
    char     session_id[64];
} spi_llm_resp_arg_t;

static void spi_llm_resp_task(void *arg)
{
    spi_llm_resp_arg_t *a = (spi_llm_resp_arg_t *)arg;
    claw_core_response_t resp = {0};
    /* Timeout must exceed worst-case LLM path: 25s WiFi timeout + ~5s retry delay + 25s WiFi
     * retry + 20s SPI proxy = ~75s. Set to 85s, safely under Pico relay timeout (90s). */
    ESP_LOGI(TAG, "spi_llm_resp: waiting for req=%u (session=%s)",
             (unsigned)a->request_id, a->session_id);
    esp_err_t err = claw_agent_mgr_receive_root_for(a->request_id, &resp, 85000);
    ESP_LOGI(TAG, "spi_llm_resp: req=%u receive err=%s text_len=%d",
             (unsigned)a->request_id, esp_err_to_name(err),
             (resp.text ? (int)strlen(resp.text) : -1));

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "session_id", a->session_id);
        if (err == ESP_OK && resp.text && resp.text[0]) {
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddStringToObject(root, "text", resp.text);
        } else {
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "text", "처리 오류");
        }
        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            uint16_t slen = (uint16_t)strlen(s);
            if (slen > SPI_CLAW_MAX_CHUNK - 1) slen = SPI_CLAW_MAX_CHUNK - 1;
            spi_wiz_send(s_spi_wiz, SPI_CMD_LLM_RESP, (const uint8_t *)s, slen);
            ESP_LOGI(TAG, "spi_llm_resp: LLM_RESP sent req=%u ok=%d slen=%u",
                     (unsigned)a->request_id,
                     (err == ESP_OK && resp.text && resp.text[0]), slen);
            free(s);
        }
        cJSON_Delete(root);
    }
    claw_core_response_free(&resp);
    free(a);
    vTaskDelete(NULL);
}

static void on_spi_rx(spi_claw_cmd_t cmd, uint8_t seq, const uint8_t *payload,
                      uint16_t len, void *ctx)
{
    (void)ctx;
    char ack[32];
    int  ack_len;

    switch (cmd) {
    case SPI_CMD_PONG:
        ESP_LOGI(TAG, "SPI PONG received from W55RP20");
        s_pong_received = true;
        break;

    case SPI_CMD_GPIO_SET: {
        if (!payload || len == 0 || len >= 256) { break; }
        char local[256];
        memcpy(local, payload, len);
        local[len] = '\0';
        int pin = -1;
        const char *state = "toggle";
        const char *pp = strstr(local, "\"pin\"");
        if (pp) { pp = strchr(pp, ':'); if (pp) { pin = atoi(pp + 1); } }
        const char *ps = strstr(local, "\"state\"");
        if (ps) {
            if (strstr(ps, "\"on\""))       state = "on";
            else if (strstr(ps, "\"off\"")) state = "off";
        }
        if (pin >= 0 && pin < 49 && pin != CONFIG_SPI_WIZ_IRQ_IO) {
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            bool cur  = (bool)gpio_get_level(pin);
            bool next = strcmp(state, "on")  == 0 ? true  :
                        strcmp(state, "off") == 0 ? false : !cur;
            gpio_set_level(pin, next ? 1 : 0);
            ESP_LOGI(TAG, "GPIO %d → %s", pin, next ? "ON" : "OFF");
        } else {
            ESP_LOGW(TAG, "GPIO_SET invalid or protected pin %d", pin);
        }
        ack_len = snprintf(ack, sizeof(ack), "{\"seq\":%u,\"ok\":true}", seq);
        spi_wiz_send(s_spi_wiz, SPI_CMD_ACK, (const uint8_t *)ack, (uint16_t)ack_len);
        break;
    }

#if CONFIG_APP_CLAW_CAP_LUA
    case SPI_CMD_LUA_EXEC: {
        if (!payload || len == 0) { break; }
        char path[128];
        if (claw_paths_join(CLAW_PATH_DATA, "spi_exec.lua", path, sizeof(path)) == ESP_OK) {
            FILE *f = fopen(path, "w");
            if (f) {
                fwrite(payload, 1, len, f);
                fclose(f);
                cap_lua_run_script_async(path, NULL, 30000, "spi_exec", NULL, true, NULL, 0);
                ESP_LOGI(TAG, "LUA_EXEC queued: %u bytes", len);
            } else {
                ESP_LOGE(TAG, "LUA_EXEC: fopen %s failed", path);
            }
        }
        ack_len = snprintf(ack, sizeof(ack), "{\"seq\":%u,\"ok\":true}", seq);
        spi_wiz_send(s_spi_wiz, SPI_CMD_ACK, (const uint8_t *)ack, (uint16_t)ack_len);
        break;
    }
#endif /* CONFIG_APP_CLAW_CAP_LUA */

    case SPI_CMD_CAPTURE_REQ:
#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
        if (xTaskCreate(s_capture_task, "spi_cap", CAPTURE_TASK_STACK,
                        NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "CAPTURE_REQ: failed to create capture task");
        }
#else
        ESP_LOGW(TAG, "CAPTURE_REQ: no camera support on this board");
#endif
        break;

    case SPI_CMD_LLM_REQ: {
        if (!payload || len == 0 || len >= SPI_CLAW_MAX_CHUNK) { break; }
        char *js = malloc(len + 1);
        if (!js) { break; }
        memcpy(js, payload, len);
        js[len] = '\0';
        cJSON *root = cJSON_Parse(js);
        free(js);
        if (!root) { ESP_LOGW(TAG, "LLM_REQ: JSON parse failed"); break; }

        const char *session_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_id"));
        const char *sender     = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sender"));
        const char *text       = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));

        if (!text || !session_id) { cJSON_Delete(root); break; }

        uint32_t request_id = 0;
        esp_err_t err = claw_agent_mgr_submit_root_text(
            text, session_id, 0, 2000, &request_id);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "LLM_REQ from '%s' session='%s' req=%u",
                     sender ? sender : "?", session_id, (unsigned)request_id);
            spi_llm_resp_arg_t *arg = malloc(sizeof(spi_llm_resp_arg_t));
            if (arg) {
                arg->request_id = request_id;
                strlcpy(arg->session_id, session_id, sizeof(arg->session_id));
                if (xTaskCreate(spi_llm_resp_task, "spi_llm", 4096, arg, 5, NULL) != pdPASS) {
                    ESP_LOGE(TAG, "LLM_REQ: spawn resp task failed");
                    free(arg);
                }
            }
        } else {
            /* Agent unavailable (e.g. LLM unconfigured → claw_core not started).
             * Reply ok=false immediately so the Pico falls back at once instead
             * of waiting out its 90s relay timeout. */
            ESP_LOGE(TAG, "LLM_REQ: submit failed %s — sending ok=false",
                     esp_err_to_name(err));
            char nack[128];
            int nlen = snprintf(nack, sizeof(nack),
                "{\"session_id\":\"%s\",\"ok\":false,\"text\":\"agent unavailable\"}",
                session_id);
            if (nlen > 0 && nlen < (int)sizeof(nack)) {
                spi_wiz_send(s_spi_wiz, SPI_CMD_LLM_RESP,
                             (const uint8_t *)nack, (uint16_t)nlen);
            }
        }
        cJSON_Delete(root);
        break;
    }

    case SPI_CMD_HTTP_RESP: {
        /* {"status":200,"body_len":N} */
        if (!payload || len == 0 || len >= 128) { break; }
        char js[128];
        memcpy(js, payload, len); js[len] = '\0';
        cJSON *root = cJSON_Parse(js);
        if (root) {
            cJSON *st = cJSON_GetObjectItem(root, "status");
            cJSON *bl = cJSON_GetObjectItem(root, "body_len");
            if (cJSON_IsNumber(st)) {
                s_http_proxy_resp_status = (int)st->valuedouble;
            }
            size_t blen = cJSON_IsNumber(bl) ? (size_t)bl->valuedouble : 0;
            if (blen > 0) {
                free(s_http_proxy_resp_body);
                s_http_proxy_resp_body = malloc(blen + 1);
                s_http_proxy_resp_cap  = blen + 1;
                s_http_proxy_resp_len  = 0;
                if (s_http_proxy_resp_body) { s_http_proxy_resp_body[0] = '\0'; }
            }
            cJSON_Delete(root);
        }
        break;
    }

    case SPI_CMD_HTTP_RESP_BODY: {
        if (!payload || len == 0) { break; }
        if (s_http_proxy_resp_body &&
            s_http_proxy_resp_len + len < s_http_proxy_resp_cap) {
            memcpy(s_http_proxy_resp_body + s_http_proxy_resp_len, payload, len);
            s_http_proxy_resp_len += len;
            s_http_proxy_resp_body[s_http_proxy_resp_len] = '\0';
        }
        break;
    }

    case SPI_CMD_HTTP_RESP_END: {
        if (s_http_proxy_sem) {
            xSemaphoreGive(s_http_proxy_sem);
        }
        break;
    }

    default:
        ESP_LOGW(TAG, "SPI unhandled cmd=0x%02X seq=%u len=%u", cmd, seq, len);
        break;
    }
}

/* ── Heartbeat (SPI_CMD_ESP_STATUS) ───────────────────────────────────
 * Sent every 20s. Carries objective health the Pico guardian uses for
 * 3-state liveness (see docs/HEARTBEAT.md, to be written):
 *   seq  : monotonic per-send counter. Advancing seq proves this task — and
 *          thus the RTOS scheduler at this priority — is still running. A
 *          frozen seq while packets still arrive would indicate a stall.
 *   up   : uptime seconds. A drop to a low value = ESP rebooted (crash/TWDT).
 *   heap : free heap now. Collapsing heap preceded the OOM crashes in
 *          docs/DEVLOG.md (10th/11th) — lets the guardian see it coming.
 *   wifi : real STA state (not hardcoded).
 *   proxy: whether the LLM transport is currently in SPI-proxy mode.
 * Note: "agent logically hung" is NOT self-reported here (a hung agent can't
 * honestly report it). The guardian infers that from relay-timeout streaks on
 * the Pico side. This payload is objective, self-consistent facts only. */
static void spi_status_task(void *arg)
{
    (void)arg;
    static uint32_t seq = 0;
    char status_json[160];
    while (1) {
        uint32_t up   = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000u);
        uint32_t heap = esp_get_free_heap_size();
        bool     prox = claw_llm_http_get_proxy_mode();
        int n = snprintf(status_json, sizeof(status_json),
                 "{\"seq\":%" PRIu32 ",\"up\":%" PRIu32 ",\"heap\":%" PRIu32
                 ",\"wifi\":%s,\"proxy\":%s}",
                 seq, up, heap,
                 s_wifi_connected ? "true" : "false",
                 prox ? "true" : "false");
        if (n > 0 && n < (int)sizeof(status_json)) {
            spi_wiz_send(s_spi_wiz, SPI_CMD_ESP_STATUS,
                         (const uint8_t *)status_json, (uint16_t)n);
        }
        ESP_LOGI(TAG, "[health] seq=%" PRIu32 " up=%" PRIu32 " free_heap=%" PRIu32
                 " min_ever=%" PRIu32 " wifi=%d proxy=%d",
                 seq, up, heap, esp_get_minimum_free_heap_size(),
                 (int)s_wifi_connected, (int)prox);
        seq++;
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}

static void spi_wiz_ping_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    int attempt = 0;
    while (!s_pong_received) {
        ESP_LOGI(TAG, "SPI PING attempt %d", ++attempt);
        spi_wiz_send(s_spi_wiz, SPI_CMD_PING, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    ESP_LOGI(TAG, "PING/PONG handshake complete after %d attempt(s)", attempt);

    /* 주기적 STATUS 브로드캐스트 — Pico가 ESP32 alive 감지에 사용 */
    xTaskCreate(spi_status_task, "spi_status", 2048, NULL, 3, NULL);

#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
    /* camera_service mutex is held during its own init; wait for it to release. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (!s_camera_opened) {
        camera_open_opts_t opts = { 0 };
        if (camera_open(CAPTURE_DEV_PATH, &opts) == ESP_OK) {
            s_camera_opened = true;
            camera_stream_info_t si = { 0 };
            if (camera_get_stream_info(&si) == ESP_OK) {
                s_cam_w = si.width  ? si.width  : 640;
                s_cam_h = si.height ? si.height : 480;
            }
            ESP_LOGI(TAG, "Camera pre-opened: %"PRIu32"x%"PRIu32, s_cam_w, s_cam_h);
#if CONFIG_PERSON_DETECT_ENABLE
            if (!person_detect_init()) {
                ESP_LOGW(TAG, "TFLite person detect init failed — will run without filtering");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Camera pre-open failed — will retry on first capture");
        }
    }
#endif
    vTaskDelete(NULL);
}
#endif /* CONFIG_SPI_CAM_BRIDGE_ENABLE */

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;
    s_wifi_connected = connected;

#if CONFIG_SPI_CAM_BRIDGE_ENABLE
    if (connected) {
        /* WiFi restored — revert to direct HTTP, proxy no longer needed */
        claw_llm_http_set_proxy_mode(false);
        ESP_LOGI(TAG, "WiFi restored, SPI proxy disabled");
    } else {
        /* WiFi lost — enable SPI proxy if Pico is present */
        claw_llm_http_set_proxy_mode(true);
        ESP_LOGW(TAG, "WiFi lost, SPI proxy enabled");
    }
#endif

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    return app_config_save(config);
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    esp_err_t ret = ESP_OK;
    cap_im_wechat_qr_login_status_t *raw = NULL;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    raw = calloc(1, sizeof(*raw));
    ESP_RETURN_ON_FALSE(raw, ESP_ERR_NO_MEM, TAG, "Failed to allocate login status");

    ESP_GOTO_ON_ERROR(cap_im_wechat_qr_login_get_status(raw), cleanup, TAG,
                      "Failed to query WeChat login status");

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

cleanup:
    free(raw);
    return ret;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error

#if CONFIG_SPI_CAM_BRIDGE_ENABLE && CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT && CONFIG_PERSON_DETECT_ENABLE
    /* Allocate TFLite arena before WiFi/board-manager grab contiguous SRAM blocks. */
    if (!person_detect_init()) {
        ESP_LOGW(TAG, "Early TFLite arena alloc failed — will retry on first capture");
    }
#endif

    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(app_claw_ui_start());
    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

#ifndef CONFIG_NETWORK_BACKEND_WIRED
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            if (wifi_manager_wait_connected(30000) == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else {
                ESP_LOGW(TAG, "STA could not connect, dropped to AP fallback");
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }
#else /* CONFIG_NETWORK_BACKEND_WIRED */
    /* lwIP and event loop are normally started by wifi_manager.
     * In wired mode we must init them manually so capabilities that
     * use TCP (Telegram, HTTP) don't assert on an uninitialized mbox. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#endif /* CONFIG_NETWORK_BACKEND_WIRED */

#if CONFIG_SPI_CAM_BRIDGE_ENABLE
    s_http_proxy_sem  = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_http_proxy_sem ? ESP_OK : ESP_ERR_NO_MEM);
    s_spi_proxy_lock  = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_spi_proxy_lock ? ESP_OK : ESP_ERR_NO_MEM);
    claw_llm_http_set_spi_proxy(spi_http_proxy_fn);

    ESP_ERROR_CHECK(spi_wiz_create(&(spi_wiz_config_t){
        .sck_io    = CONFIG_SPI_WIZ_SCK_IO,
        .miso_io   = CONFIG_SPI_WIZ_MISO_IO,
        .mosi_io   = CONFIG_SPI_WIZ_MOSI_IO,
        .cs_io     = CONFIG_SPI_WIZ_CS_IO,
        .irq_io    = CONFIG_SPI_WIZ_IRQ_IO,
        .clock_hz  = CONFIG_SPI_WIZ_CLOCK_HZ,
        .on_rx     = on_spi_rx,
        .on_rx_ctx = NULL,
    }, &s_spi_wiz));
    ESP_ERROR_CHECK(spi_wiz_start(s_spi_wiz));
    xTaskCreate(spi_wiz_ping_task, "spi_ping", 4096, NULL, 5, NULL);

    /* PIR: task first so handle is valid before ISR fires */
    xTaskCreate(pir_send_task, "pir_send", 2048, NULL, 5, &s_pir_task_handle);
    {
        gpio_config_t pir_cfg = {
            .pin_bit_mask = (1ULL << PIR_GPIO_PIN),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_POSEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&pir_cfg));
        /* ISR service may already be installed by camera driver — ignore INVALID_STATE */
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(isr_err);
        }
        ESP_ERROR_CHECK(gpio_isr_handler_add(PIR_GPIO_PIN, pir_isr_handler, NULL));
        ESP_LOGI(TAG, "PIR sensor GPIO%d ready (debounce=%dms)", PIR_GPIO_PIN, PIR_DEBOUNCE_MS);
    }
#endif /* CONFIG_SPI_CAM_BRIDGE_ENABLE */

    ESP_ERROR_CHECK(app_claw_start(s_claw_config));
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
