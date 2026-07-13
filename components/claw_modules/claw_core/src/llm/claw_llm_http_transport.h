/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "llm/claw_llm_types.h"

esp_err_t claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
                                  claw_llm_http_response_t *out_response,
                                  char **out_error_message);
void claw_llm_http_response_free(claw_llm_http_response_t *response);

/* ── SPI proxy transport ──────────────────────────────────────────────
 * When registered, HTTP POST requests are forwarded to Pico via SPI
 * instead of going directly over WiFi.
 *
 * fn(url, auth_header, body_json, &out_body, &out_status)
 *   out_body   — heap-allocated response body (caller frees)
 *   out_status — HTTP status code (200 = success)
 */
typedef esp_err_t (*claw_llm_spi_proxy_fn_t)(const char *url,
                                              const char *auth_header,
                                              const char *body_json,
                                              char      **out_body,
                                              int        *out_status);

void claw_llm_http_set_spi_proxy(claw_llm_spi_proxy_fn_t fn);
void claw_llm_http_set_proxy_mode(bool enabled);
bool claw_llm_http_get_proxy_mode(void);
