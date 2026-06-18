/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WIZ_CLAW_SPI_PROTO_H
#define WIZ_CLAW_SPI_PROTO_H

#include <stdint.h>

#define SPI_CLAW_MAGIC_0   0xCA
#define SPI_CLAW_MAGIC_1   0xFE
#define SPI_CLAW_MAX_CHUNK 4096u   /* max payload per packet */
#define SPI_CLAW_HDR_SIZE  7u      /* sizeof(spi_claw_hdr_t) */

typedef enum {
    /* ── W55RP20 → ESP32 (downstream) ───────────── */
    SPI_CMD_PING         = 0x01,
    SPI_CMD_LUA_EXEC     = 0x02,  /* payload: Lua script string */
    SPI_CMD_GPIO_SET     = 0x03,  /* payload: {"pin":N,"state":"on|off|toggle"} */
    SPI_CMD_CAPTURE_REQ  = 0x04,  /* payload: none (request capture + chunk send) */
    SPI_CMD_LLM_REQ      = 0x05,  /* payload: {"session_id":"tg_<chat_id>","chat_id":"...","sender":"...","text":"..."} */

    /* ── ESP32 → W55RP20 (upstream) ─────────────── */
    SPI_CMD_EVENT        = 0x40,  /* payload: {"type":"motion","label":"cat",...} */
    SPI_CMD_CHUNK_DATA   = 0x41,  /* payload: {"seq":N,"total":T,"data":[...]} */
    SPI_CMD_CHUNK_END    = 0x42,  /* payload: {"mime":"image/jpeg","size":N} */
    SPI_CMD_LLM_RESP     = 0x43,  /* payload: {"session_id":"...","ok":true,"text":"..."} */
    SPI_CMD_ESP_STATUS   = 0x44,  /* payload: {"wifi":true,"agent":true} */

    /* ── bidirectional ──────────────────────────── */
    SPI_CMD_ACK          = 0x80,  /* payload: {"seq":N,"ok":true} */
    SPI_CMD_NACK         = 0x81,  /* payload: {"seq":N,"error":"..."} */
    SPI_CMD_PONG         = 0x82,  /* CMD_PING response */
} spi_claw_cmd_t;

/* Fixed 7-byte header — identical layout on both sides */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];  /* SPI_CLAW_MAGIC_0, SPI_CLAW_MAGIC_1 */
    uint8_t  cmd;       /* spi_claw_cmd_t */
    uint16_t len;       /* payload length, little-endian */
    uint8_t  seq;       /* sequence number (ACK matching) */
    uint8_t  crc;       /* XOR checksum of header+payload (crc field = 0 during calc) */
} spi_claw_hdr_t;

#endif /* WIZ_CLAW_SPI_PROTO_H */
