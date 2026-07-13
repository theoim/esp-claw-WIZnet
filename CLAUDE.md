# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> Core rules live in [`AGENTS.md`](./AGENTS.md). Read it first — this file adds context that AGENTS.md does not cover.

## Build Environment

**Claude Code cannot run `idf.py` directly.** The ESP-IDF export script (`export.sh` / `export.bat`) is not sourced in Claude's shell. All firmware builds must be triggered by the user from a VS Code ESP-IDF terminal or a shell where IDF is exported.

When a build step is required, provide the exact command and ask the user to run it:

```bash
# From application/edge_agent/ with IDF exported:
idf.py bmgr -c ./boards -b <board_name>
idf.py build
idf.py flash monitor
```

There are no automated tests that Claude can run; test coverage means `idf.py build` succeeds and the device boots cleanly.

## Repository Layout

```
esp-claw/
├── application/edge_agent/     # Main IDF app (target: ESP32-S3/P4/C5/S31)
│   ├── main/                   # App entry (main.c, Kconfig.projbuild, app_fs.c)
│   ├── components/
│   │   ├── app_config/         # NVS-backed config key/value store
│   │   ├── http_server/        # Config web UI (C backend + React/TS frontend)
│   │   ├── spi_wiz/            # SPI master driver for W55RP20 bridge
│   │   └── gen_bmgr_codes/     # Board manager generated code (do not edit by hand)
│   ├── boards/                 # Board definitions (YAML + setup_device.c + sdkconfig.defaults.board)
│   └── fatfs_image/            # Build-time FATFS content (system/ + storage/)
├── components/
│   └── claw_modules/
│       ├── claw_core/          # Agent loop, LLM runtime, context building, tool calls
│       ├── claw_cap/           # Capability registry and dispatch
│       ├── claw_event_router/  # Declarative event routing via router_rules.json
│       ├── claw_memory/        # Session history, long-term memory, profile
│       ├── claw_manager/       # Agent manager, session manager
│       ├── claw_skill/         # Skill document registry
│       └── claw_ramfs/         # In-RAM filesystem abstraction
└── docs/                       # Documentation site (pnpm / Astro)
```

## SPI Bridge (W55RP20 / RP2350 "Pico" Side)

This repo is deployed with an optional wired-network extension board: a **W55RP20** (RP2350 + WIZnet W6300 Ethernet). The ESP32 acts as SPI master; the Pico acts as SPI slave and runs the WIZnet-PICO-C project.

### ESP32 side

- **Driver**: `application/edge_agent/components/spi_wiz/spi_wiz.c` — `spi_wiz_handle_t`
- **SPI mode 1** (CPOL=0, CPHA=1) at 8 MHz — workaround for RP2350 Errata E14
- IRQ line: Pico GP4 → ESP32 GPIO2 (POSEDGE) — Pico asserts high while slave has data to send
- `s_rx_task` (prio 10) wakes on `irq_sem` and reads one full packet (header + payload) under `tx_mutex`
- `spi_wiz_send()` also holds `tx_mutex` — the mutex serializes ALL `spi_device_transmit` calls to prevent the IDF assert at `spi_master.c:1310`
- HTTP proxy flow: `spi_http_proxy_fn` in `main/main.c` — reset/drain semaphore **before** sending the request, not after

### Protocol

Shared header: `application/edge_agent/components/spi_wiz/include/wiz_claw_spi_proto.h`

```
spi_claw_hdr_t (7 bytes, packed):
  magic[2]  0xCA 0xFE
  cmd       spi_claw_cmd_t
  len       uint16_t  payload length (≤ 4096)
  seq       uint8_t
  crc       XOR of all header bytes (crc=0 during calc) + all payload bytes
```

Key commands: `SPI_CMD_LLM_REQ (0x05)` · `HTTP_REQ/BODY/BODY_END (0x06-0x08)` from Pico→ESP32; `SPI_CMD_LLM_RESP (0x43)` · `HTTP_RESP/BODY/END (0x45-0x47)` from ESP32→Pico.

### Pico side

Source at `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\` (separate git repo, not part of this IDF project).

- Main file: `examples/wiz_claw_spi_host/main.c`
- SPI slave handler with CRC error callback: `on_spi_crc_error()` sets `g_proxy_crc_err` flag — never calls `_proxy_send_response()` mid-stream; defers 500 to `SPI_CMD_HTTP_BODY_END` handler
- HTTP client: `port/wiz-claw/src/wiz_claw_http.c` — raw TCP over WIZnet W6300 sockets (no TLS)
- Build: Pico SDK (`cmake -B build && cmake --build build`), flash via `picotool`

## Kconfig: Network Backend

`application/edge_agent/main/Kconfig.projbuild` defines:

- `NETWORK_BACKEND_WIFI` (default) — standard ESP-IDF WiFi
- `NETWORK_BACKEND_WIRED` — activates `g_use_spi_proxy` and `spi_wiz` driver; SPI GPIO pins configured here
- `SPI_CAM_BRIDGE_ENABLE` — enables JPEG camera streaming to Pico and optional TFLite person detection

## Board Selection

```bash
idf.py bmgr -c ./boards -b <vendor>/<board_name>
# e.g. -b espressif/esp32_S3_DevKitC_1
```

`bmgr` generates `components/gen_bmgr_codes/` from the board YAML files. Never hand-edit generated files there. Board-specific `sdkconfig.defaults.board` is merged automatically.
