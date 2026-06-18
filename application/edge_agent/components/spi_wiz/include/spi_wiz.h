/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "wiz_claw_spi_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spi_wiz_t *spi_wiz_handle_t;

typedef struct {
    int  sck_io;    /* SCK GPIO  (default: GPIO7)  */
    int  miso_io;   /* MISO GPIO (default: GPIO8)  */
    int  mosi_io;   /* MOSI GPIO (default: GPIO9)  */
    int  cs_io;     /* CS GPIO   (default: GPIO1)  */
    int  irq_io;    /* IRQ GPIO  (default: GPIO2, W55RP20→ESP32 data-ready) */
    int  clock_hz;  /* SPI clock Hz (e.g. 8000000) */
    void (*on_rx)(spi_claw_cmd_t cmd, uint8_t seq, const uint8_t *payload,
                  uint16_t len, void *ctx);
    void *on_rx_ctx;
} spi_wiz_config_t;

/**
 * @brief  Allocate and configure the SPI master + IRQ GPIO.
 *         Does not start the receive task.
 */
esp_err_t spi_wiz_create(const spi_wiz_config_t *cfg, spi_wiz_handle_t *ret);

/**
 * @brief  Start the background receive task.
 *         Call after spi_wiz_create.
 */
esp_err_t spi_wiz_start(spi_wiz_handle_t h);

/**
 * @brief  Stop the receive task and release all resources.
 */
esp_err_t spi_wiz_delete(spi_wiz_handle_t h);

/**
 * @brief  Build a framed packet and transmit it over SPI.
 *         Thread-safe; may be called from any task.
 *
 * @param payload  May be NULL when len == 0.
 */
esp_err_t spi_wiz_send(spi_wiz_handle_t h, spi_claw_cmd_t cmd,
                        const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
