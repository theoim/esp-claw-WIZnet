/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "spi_wiz.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "spi_wiz";

#define SPI_WIZ_RX_TASK_STACK 4096
#define SPI_WIZ_RX_TASK_PRIO  10

struct spi_wiz_t {
    spi_device_handle_t  spi_dev;
    spi_wiz_config_t     cfg;
    SemaphoreHandle_t    irq_sem;
    SemaphoreHandle_t    tx_mutex;
    TaskHandle_t         rx_task;
    uint8_t              tx_seq;
};

/* XOR checksum: caller must zero the crc field in hdr_buf before calling */
static uint8_t s_calc_crc(const uint8_t *hdr_buf, const uint8_t *payload, uint16_t plen)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < SPI_CLAW_HDR_SIZE; i++) {
        crc ^= hdr_buf[i];
    }
    for (uint16_t i = 0; i < plen; i++) {
        crc ^= payload[i];
    }
    return crc;
}

static void IRAM_ATTR s_irq_isr(void *arg)
{
    spi_wiz_handle_t h = (spi_wiz_handle_t)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(h->irq_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Zero-filled TX placeholder — prevents W-register stale data on MOSI during RX.
 * Not const: rodata may be placed in flash which is not SPI-DMA accessible. */
static uint8_t s_tx_zeros[SPI_CLAW_MAX_CHUNK];

static void s_rx_task(void *arg)
{
    spi_wiz_handle_t h = (spi_wiz_handle_t)arg;

    /* Static buffers: DMA requires src/dst in internal SRAM.
     * Stack could be in IRAM which is also fine, but static is guaranteed DRAM. */
    static uint8_t   hdr_buf[SPI_CLAW_HDR_SIZE];
    static uint8_t   tx_dummy[SPI_CLAW_HDR_SIZE];  /* zero by BSS init */
    static uint8_t  *pay_buf;
    static uint16_t  pay_cap;

    while (1) {
        if (xSemaphoreTake(h->irq_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Read fixed-length header */
        spi_transaction_t th = {
            .length    = SPI_CLAW_HDR_SIZE * 8,
            .tx_buffer = tx_dummy,
            .rx_buffer = hdr_buf,
        };
        if (spi_device_transmit(h->spi_dev, &th) != ESP_OK) {
            ESP_LOGW(TAG, "Header RX failed");
            continue;
        }

        spi_claw_hdr_t *hdr = (spi_claw_hdr_t *)hdr_buf;
        if (hdr->magic[0] != SPI_CLAW_MAGIC_0 || hdr->magic[1] != SPI_CLAW_MAGIC_1) {
            ESP_LOGW(TAG, "Bad magic: 0x%02X 0x%02X", hdr->magic[0], hdr->magic[1]);
            continue;
        }

        uint16_t plen = hdr->len;
        if (plen > SPI_CLAW_MAX_CHUNK) {
            ESP_LOGW(TAG, "Payload too large: %u", plen);
            continue;
        }

        /* Grow payload buffer on demand; never shrink. DMA requires MALLOC_CAP_INTERNAL. */
        if (plen > pay_cap) {
            heap_caps_free(pay_buf);
            pay_buf = heap_caps_malloc(plen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!pay_buf) {
                ESP_LOGE(TAG, "OOM for payload %u bytes", plen);
                pay_cap = 0;
                continue;
            }
            pay_cap = plen;
        }

        if (plen > 0) {
            spi_transaction_t tp = {
                .length    = plen * 8,
                .tx_buffer = s_tx_zeros,   /* explicit zeros — prevent W-reg stale data on MOSI */
                .rx_buffer = pay_buf,
            };
            if (spi_device_transmit(h->spi_dev, &tp) != ESP_OK) {
                ESP_LOGW(TAG, "Payload RX failed");
                continue;
            }
        }

        /* Verify CRC: zero crc field, compute, restore */
        uint8_t recv_crc = hdr_buf[offsetof(spi_claw_hdr_t, crc)];
        hdr_buf[offsetof(spi_claw_hdr_t, crc)] = 0;
        uint8_t calc_crc = s_calc_crc(hdr_buf, pay_buf, plen);
        hdr_buf[offsetof(spi_claw_hdr_t, crc)] = recv_crc;

        if (calc_crc != recv_crc) {
            ESP_LOGW(TAG, "CRC mismatch: got 0x%02X expected 0x%02X", recv_crc, calc_crc);
            continue;
        }

        if (h->cfg.on_rx) {
            h->cfg.on_rx((spi_claw_cmd_t)hdr->cmd, hdr->seq,
                         plen ? pay_buf : NULL, plen, h->cfg.on_rx_ctx);
        }
    }
}

esp_err_t spi_wiz_create(const spi_wiz_config_t *cfg, spi_wiz_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret, ESP_ERR_INVALID_ARG, TAG, "null arg");

    spi_wiz_handle_t h = calloc(1, sizeof(struct spi_wiz_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "OOM");

    h->cfg = *cfg;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = cfg->mosi_io,
        .miso_io_num     = cfg->miso_io,
        .sclk_io_num     = cfg->sck_io,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SPI_CLAW_HDR_SIZE + SPI_CLAW_MAX_CHUNK,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        free(h);
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = cfg->clock_hz,
        .mode           = 1,   /* CPOL=0 CPHA=1: RP2350 CPHA=0 errata 회피 */
        .spics_io_num   = cfg->cs_io,
        .queue_size     = 4,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &h->spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        free(h);
        return err;
    }

    h->irq_sem  = xSemaphoreCreateBinary();
    h->tx_mutex = xSemaphoreCreateMutex();
    if (!h->irq_sem || !h->tx_mutex) {
        err = ESP_ERR_NO_MEM;
        goto cleanup_spi;
    }

    gpio_config_t irq_cfg = {
        .pin_bit_mask = (1ULL << cfg->irq_io),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    err = gpio_config(&irq_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        goto cleanup_sem;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means already installed by another driver — acceptable */
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        goto cleanup_sem;
    }

    err = gpio_isr_handler_add(cfg->irq_io, s_irq_isr, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add: %s", esp_err_to_name(err));
        goto cleanup_sem;
    }

    *ret = h;
    return ESP_OK;

cleanup_sem:
    if (h->irq_sem)  { vSemaphoreDelete(h->irq_sem); }
    if (h->tx_mutex) { vSemaphoreDelete(h->tx_mutex); }
cleanup_spi:
    spi_bus_remove_device(h->spi_dev);
    spi_bus_free(SPI2_HOST);
    free(h);
    return err;
}

esp_err_t spi_wiz_start(spi_wiz_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    BaseType_t ok = xTaskCreate(s_rx_task, "spi_wiz_rx", SPI_WIZ_RX_TASK_STACK,
                                 h, SPI_WIZ_RX_TASK_PRIO, &h->rx_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create rx task");
    return ESP_OK;
}

esp_err_t spi_wiz_delete(spi_wiz_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    if (h->rx_task) {
        vTaskDelete(h->rx_task);
        h->rx_task = NULL;
    }
    gpio_isr_handler_remove(h->cfg.irq_io);
    vSemaphoreDelete(h->irq_sem);
    vSemaphoreDelete(h->tx_mutex);
    spi_bus_remove_device(h->spi_dev);
    spi_bus_free(SPI2_HOST);
    free(h);
    return ESP_OK;
}

esp_err_t spi_wiz_send(spi_wiz_handle_t h, spi_claw_cmd_t cmd,
                        const uint8_t *payload, uint16_t len)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(len == 0 || payload, ESP_ERR_INVALID_ARG, TAG, "null payload with non-zero len");
    ESP_RETURN_ON_FALSE(len <= SPI_CLAW_MAX_CHUNK, ESP_ERR_INVALID_SIZE, TAG, "payload exceeds max chunk");

    uint16_t total = (uint16_t)(SPI_CLAW_HDR_SIZE + len);
    uint8_t *buf   = heap_caps_malloc(total, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "OOM");

    spi_claw_hdr_t *hdr = (spi_claw_hdr_t *)buf;
    hdr->magic[0] = SPI_CLAW_MAGIC_0;
    hdr->magic[1] = SPI_CLAW_MAGIC_1;
    hdr->cmd      = (uint8_t)cmd;
    hdr->len      = len;
    hdr->seq      = h->tx_seq++;
    hdr->crc      = 0;

    if (len > 0) {
        memcpy(buf + SPI_CLAW_HDR_SIZE, payload, len);
    }
    /* CRC over header (crc=0) + payload */
    hdr->crc = s_calc_crc(buf, buf + SPI_CLAW_HDR_SIZE, len);

    xSemaphoreTake(h->tx_mutex, portMAX_DELAY);
    spi_transaction_t t = {
        .length    = total * 8,
        .tx_buffer = buf,
    };
    esp_err_t err = spi_device_transmit(h->spi_dev, &t);
    xSemaphoreGive(h->tx_mutex);

    heap_caps_free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_transmit: %s", esp_err_to_name(err));
    }
    return err;
}
