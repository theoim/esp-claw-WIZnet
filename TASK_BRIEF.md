# TASK_BRIEF: ESP32-S3 + W55RP20 분산 AI 엣지 에이전트

> 이 문서는 CLI 에이전트 작업지시서다.
> `/understand` 실행 후 이 파일을 읽고 구현을 시작한다.
> 레퍼런스 저장소 두 곳을 모두 읽을 것.

---

## 0. 레포지터리 경로

| 역할 | 경로 |
|------|------|
| ESP32 앱 (호스트 MCU) | `D:\ESP-IDF\esp-claw\application\edge_agent` |
| ESP32 공유 컴포넌트 | `D:\ESP-IDF\esp-claw\components\` |
| RP2040 레퍼런스 예제 | `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_agent` |
| RP2040 신규 예제 (작업대상) | `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host` |

코드 스타일·아키텍처 규칙 전문: `D:\ESP-IDF\esp-claw\AGENTS.md` — 반드시 읽을 것.

---

## 1. 프로젝트 목표

현재 `edge_agent`는 ESP32-S3 단독으로 WiFi + Telegram + LLM + 카메라를 처리한다.
목표는 네트워크 스택을 W55RP20(RP2040 + W5500)으로 오프로딩하는 것이다.

```
현재:
  ESP32-S3 → WiFi → Telegram / LLM API

목표:
  ESP32-S3 (호스트 MCU)
    카메라 캡처 / 엣지 ML 추론 / Lua 실행 / GPIO·센서
    ↕ SPI (마스터) + IRQ핀
  W55RP20 (네트워크 코프로세서)
    W5500 Ethernet → Telegram 폴링 → LLM API 호출 → tool_call 파싱
```

빌드타임 매크로(`CONFIG_NETWORK_BACKEND_WIRED`)로 WiFi/유선 전환 가능.
유선 모드에서 ESP32는 네트워크 스택을 전혀 몰라야 한다.

---

## 2. 역할 분담 요약

### ESP32-S3 (호스트 MCU, SPI 마스터)
- 카메라 캡처 (OV3660 DVP)
- JPEG 인코딩
- On-device ML 추론 (Phase 5, 선택)
- Lua 스크립트 실행 (기존 런타임 재사용)
- GPIO / 디스플레이 / 센서
- 이벤트 감지 시 W55RP20에 SPI 업스트림 전송
- W55RP20에서 tool_call 수신 → 실행

### W55RP20 / RP2040 (네트워크 코프로세서, SPI 슬레이브)
- W5500 Ethernet (SPI0, 내부)
- Telegram 폴링 (기존 `wiz_claw_agent` 코드 재사용)
- LLM API 호출 (기존 `wiz_claw_llm` 재사용)
- LLM tool_call 응답 → IRQ핀 assert → ESP32에 SPI 전송
- ESP32로부터 이미지 청크 수신 → W5500 소켓으로 직접 스트리밍

---

## 3. 하드웨어 연결 (XIAO ESP32-S3 Sense ↔ W55RP20)

```
XIAO ESP32-S3          W55RP20 RP2040
─────────────          ──────────────
D8  GPIO7   SCK   ──── SPI1_SCK  GP10
D9  GPIO8   MISO  ──── SPI1_TX   GP11   (RP2040 입장 TX = MISO)
D10 GPIO9   MOSI  ──── SPI1_RX   GP12   (RP2040 입장 RX = MOSI)
D0  GPIO1   CS    ──── SPI1_CSn  GP13
D1  GPIO2   IRQ   ──── GP14             (W55RP20→ESP32 데이터준비 인터럽트)
GND                     GND
```

> 실제 W55RP20 breakout 핀아웃 확인 후 GP 번호 조정 필요.
> XIAO ESP32-S3 카메라 핀(GPIO10,13,15,16,17,18,38,47,48)과 충돌 없음 확인됨.

---

## 4. 공통 SPI 프로토콜

아래 헤더는 ESP32와 RP2040 양쪽에 **동일하게** 배포한다.

```c
/* wiz_claw_spi_proto.h */
#ifndef WIZ_CLAW_SPI_PROTO_H
#define WIZ_CLAW_SPI_PROTO_H

#include <stdint.h>

#define SPI_CLAW_MAGIC_0   0xCA
#define SPI_CLAW_MAGIC_1   0xFE
#define SPI_CLAW_MAX_CHUNK 4096u   /* 단일 패킷 최대 페이로드 */
#define SPI_CLAW_HDR_SIZE  7u      /* sizeof(spi_claw_hdr_t) */

typedef enum {
    /* ── W55RP20 → ESP32 (다운스트림) ───────────── */
    SPI_CMD_PING         = 0x01,
    SPI_CMD_LUA_EXEC     = 0x02,  /* payload: Lua 스크립트 문자열 */
    SPI_CMD_GPIO_SET     = 0x03,  /* payload: {"pin":N,"state":"on|off|toggle"} */
    SPI_CMD_CAPTURE_REQ  = 0x04,  /* payload: 없음 (캡처 후 CHUNK 전송 요청) */

    /* ── ESP32 → W55RP20 (업스트림) ─────────────── */
    SPI_CMD_EVENT        = 0x40,  /* payload: {"type":"motion","label":"cat",...} */
    SPI_CMD_CHUNK_DATA   = 0x41,  /* payload: {"seq":N,"total":T,"data":[...]} */
    SPI_CMD_CHUNK_END    = 0x42,  /* payload: {"mime":"image/jpeg","size":N} */

    /* ── 양방향 ──────────────────────────────────── */
    SPI_CMD_ACK          = 0x80,  /* payload: {"seq":N,"ok":true} */
    SPI_CMD_NACK         = 0x81,  /* payload: {"seq":N,"error":"..."} */
    SPI_CMD_PONG         = 0x82,  /* CMD_PING 응답 */
} spi_claw_cmd_t;

/* 고정 7바이트 헤더 — 양쪽 동일 구조체 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];  /* SPI_CLAW_MAGIC_0, SPI_CLAW_MAGIC_1 */
    uint8_t  cmd;       /* spi_claw_cmd_t */
    uint16_t len;       /* payload 길이, little-endian */
    uint8_t  seq;       /* 시퀀스 번호 (ACK 매칭) */
    uint8_t  crc;       /* 헤더+페이로드 XOR 체크섬 */
} spi_claw_hdr_t;

#endif /* WIZ_CLAW_SPI_PROTO_H */
```

CRC 계산: 헤더의 crc 필드를 0으로 두고 `hdr + payload` 전체 바이트 XOR.

---

## 5. 구현 단계

### Phase 1 — SPI 기반 통신 검증 (현재 작업 대상)

**완료 기준:** ESP32와 RP2040 사이에서 `CMD_PING` → `CMD_PONG` 왕복 성공.
시리얼 양쪽에서 확인 가능해야 함.

---

#### 5-A. 신규 파일: `D:\ESP-IDF\esp-claw\application\edge_agent\components\spi_wiz\`

esp-claw 코드 스타일 규칙 (AGENTS.md) 준수:
- opaque handle 패턴 (`spi_wiz_handle_t`)
- `esp_err_t` 반환
- `spi_wiz_create` / `spi_wiz_delete` / `spi_wiz_send` / `spi_wiz_start`
- 공유 상태 mutex 보호

```
components/spi_wiz/
├── CMakeLists.txt
├── include/
│   ├── spi_wiz.h            ← 공개 API
│   └── wiz_claw_spi_proto.h ← 공통 프로토콜 헤더 (4항 내용 그대로)
└── spi_wiz.c                ← SPI2 마스터 + IRQ ISR + 수신 태스크
```

`spi_wiz.h` 공개 API (최소):
```c
typedef struct spi_wiz_t *spi_wiz_handle_t;

typedef struct {
    int  sck_io;   /* GPIO7  */
    int  miso_io;  /* GPIO8  */
    int  mosi_io;  /* GPIO9  */
    int  cs_io;    /* GPIO1  */
    int  irq_io;   /* GPIO2  */
    int  clock_hz; /* 예: 8000000 (8MHz) */
    /* 수신 패킷 콜백 */
    void (*on_rx)(spi_claw_cmd_t cmd, const uint8_t *payload,
                  uint16_t len, void *ctx);
    void *on_rx_ctx;
} spi_wiz_config_t;

esp_err_t spi_wiz_create(const spi_wiz_config_t *cfg, spi_wiz_handle_t *ret);
esp_err_t spi_wiz_delete(spi_wiz_handle_t h);
esp_err_t spi_wiz_start(spi_wiz_handle_t h);
esp_err_t spi_wiz_send(spi_wiz_handle_t h, spi_claw_cmd_t cmd,
                       const uint8_t *payload, uint16_t len);
```

`spi_wiz.c` 구현 포인트:
- `spi_bus_initialize(SPI2_HOST, ...)` + `spi_bus_add_device(...)`
- IRQ GPIO: `gpio_install_isr_service` + `gpio_isr_handler_add` → 세마포어 give
- 수신 태스크: 세마포어 wait → `spi_device_transmit` (dummy TX, 실제 RX) → 헤더 검증 → 콜백
- 전송: `spi_wiz_send` 내부에서 헤더 빌드 + CRC 계산 + `spi_device_transmit`
- 스택 크기 4096, 우선순위 10

`CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "spi_wiz.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver" "esp_common" "freertos"
)
```

---

#### 5-B. edge_agent Kconfig 추가

`application/edge_agent/main/Kconfig.projbuild` 에 추가:
```kconfig
menu "Network Backend"
    choice NETWORK_BACKEND
        prompt "Network backend"
        default NETWORK_BACKEND_WIFI
        config NETWORK_BACKEND_WIFI
            bool "WiFi (default)"
        config NETWORK_BACKEND_WIRED
            bool "Wired via W55RP20 SPI"
    endchoice

    if NETWORK_BACKEND_WIRED
        config SPI_WIZ_SCK_IO
            int "SPI SCK GPIO"
            default 7
        config SPI_WIZ_MISO_IO
            int "SPI MISO GPIO"
            default 8
        config SPI_WIZ_MOSI_IO
            int "SPI MOSI GPIO"
            default 9
        config SPI_WIZ_CS_IO
            int "SPI CS GPIO"
            default 1
        config SPI_WIZ_IRQ_IO
            int "IRQ GPIO (W55RP20→ESP32)"
            default 2
        config SPI_WIZ_CLOCK_HZ
            int "SPI clock Hz"
            default 8000000
    endif
endmenu
```

---

#### 5-C. main.c Phase 1 최소 연결

`application/edge_agent/main/main.c`에서 `CONFIG_NETWORK_BACKEND_WIRED` 시:
- `spi_wiz_create` + `spi_wiz_start` 호출
- on_rx 콜백: `CMD_PONG` 수신 시 `ESP_LOGI` 출력
- 부팅 후 3초 뒤 `CMD_PING` 한 번 전송

기존 WiFi 초기화(`app_wifi_start` 등)는 `#ifndef CONFIG_NETWORK_BACKEND_WIRED`로 감쌀 것.

---

#### 5-D. 신규 RP2040 예제: `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host\`

레퍼런스: `examples/wiz_claw_agent/main.c` — 구조 동일하게 유지.

추가 파일:
```
wiz_claw_spi_host/
├── CMakeLists.txt
├── main.c               ← wiz_claw_agent/main.c 복사 후 수정
├── wiz_spi_slave.h
├── wiz_spi_slave.c      ← SPI1 슬레이브 드라이버
└── wiz_claw_spi_proto.h ← 동일 헤더 복사
```

`wiz_spi_slave.c` 구현 포인트:
- SPI1 slave mode 초기화 (`spi_init(spi1, ...)`, `spi_set_slave(spi1, ...)`)
- IRQ핀 (GP14): `gpio_put(IRQ_PIN, 1)` → ESP32에 데이터 준비 알림 → `spi_write_read_blocking` → `gpio_put(IRQ_PIN, 0)`
- 헤더 파싱: magic 검증 → len 읽기 → payload 수신 → CRC 검증 → cmd dispatch
- CMD_PING 수신 시 CMD_PONG 응답 즉시 전송
- 수신 루프는 `__wfe()` (저전력 대기) 기반 폴링

`main.c` 수정:
- `wiz_spi_slave_init()` 추가
- tool_handler에서 향후 Phase 2용 분기 주석 추가 (`// TODO: Phase 2 SPI forward`)
- 메인 루프에 `wiz_spi_slave_poll()` 호출 추가 (Telegram 폴링과 인터리빙)

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(wiz_claw_spi_host C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(wiz_claw_spi_host
    main.c
    wiz_spi_slave.c
)
target_include_directories(wiz_claw_spi_host PRIVATE .)
target_link_libraries(wiz_claw_spi_host
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_dma
)
# wiz_claw_agent 라이브러리 연결 (레퍼런스 예제와 동일)
# target_link_libraries(... wiz_claw_net wiz_claw_http wiz_claw_telegram wiz_claw_llm wiz_claw_agent)
pico_enable_stdio_usb(wiz_claw_spi_host 1)
pico_add_extra_outputs(wiz_claw_spi_host)
```

---

### Phase 2 — 커맨드 실행 (다음 단계, 지금 구현 안 함)
- RP2040 tool_handler → SPI_CMD_LUA_EXEC / SPI_CMD_GPIO_SET 전송
- ESP32 디스패처 → 기존 Lua 런타임 직접 호출
- ACK 반환

### Phase 3 — 카메라 캡처 전송 (다음 단계, 지금 구현 안 함)
- SPI_CMD_CAPTURE_REQ → ESP32 캡처 → JPEG 청크 업스트림
- RP2040 청크 조립 → W5500 소켓 직접 스트리밍 → Telegram sendPhoto

---

## 6. 빌드 검증 방법

### ESP32
```powershell
# D:\ESP-IDF\esp-claw\application\edge_agent 에서
idf.py set-target esp32s3
idf.py menuconfig   # Network Backend → Wired 선택
idf.py build
```
오류 없이 빌드 완료되면 Phase 1 ESP32 파트 완료.

### RP2040
```bash
# D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host 에서
mkdir build && cd build
cmake .. -DPICO_BOARD=wiznet_w55rp20_evb_pico  # 또는 해당 보드명
make -j4
```
`.uf2` 생성되면 Phase 1 RP2040 파트 완료.

---

## 7. 참조 파일 목록

| 파일 | 용도 |
|------|------|
| `examples/wiz_claw_agent/main.c` | RP2040 에이전트 구조 레퍼런스 |
| `edge_agent/main/main.c` | ESP32 부팅 플로우 레퍼런스 |
| `edge_agent/main/Kconfig.projbuild` | 기존 Kconfig 패턴 참조 |
| `components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c` | HTTP transport 패턴 (Phase 3 참조) |
| `AGENTS.md` | 코드 스타일 전체 규칙 |

---

## 8. 주의사항

- AGENTS.md의 코드 스타일 규칙을 반드시 따른다 (opaque handle, esp_err_t 반환 등).
- `edge_agent/main/main.c` 수정 시 기존 WiFi 경로를 `#ifndef CONFIG_NETWORK_BACKEND_WIRED`로만 감싼다. 기존 코드 삭제 금지.
- RP2040 SPI1과 W5500 내부 SPI0은 별개 하드웨어다. 충돌 없음.
- XIAO ESP32-S3의 SPI2(GPIO7/8/9)는 카메라 핀과 겹치지 않는다.
- Phase 1에서 실제 하드웨어 연결 없이도 `idf.py build` 빌드 성공이 1차 목표다.
