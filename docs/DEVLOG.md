# DEVLOG

날짜별 개발 로그. 최신 항목이 위에 온다.

---

## 2026-07-13 (S-트랙 14차) — 재설정 후 릴레이 5/5 성공, 12차 미재현, 첫 지연 수치

LLM 재설정(프로비저닝 포털) 후 시나리오 2 재실행. **relay_ok=5/5, 크래시 0, 힙 안정**
(ESP free_heap 5.61M 부근 복귀 반복, min_ever 5.45M; Pico heap 477736 고정).

**12차 "정답 미전달" 미재현.** 신규 계측 로그로 5회 전부
`receive err=ESP_OK text_len=N` → `LLM_RESP sent ok=1` → Pico `RX cmd=0x43` 정상.
지난번 미전달은 fatfs-full 직전 상태의 세션 매니저 이상일 가능성 — 원인 미확정,
카운터(relay_to)로 재발 감시 지속.

**세션 메모리 릴레이 경유 동작 확인**: "내이름은 theo야" → "내이름이 뭐라고?" →
"테오님이라고요!" (Session History context_len 36→610 증가).

**첫 지연 실측 (ESP 로그 타임스탬프 기준, LLM_REQ 수신→LLM_RESP 송신)**:
| req | 경로 | 왕복 | 비고 |
|-----|------|------|------|
| 1 | WiFi 직결 | 3.1s | 베이스라인 |
| 2 | WiFi 실패→프록시 재시도 | **40.1s** | TLS write -0x0050(연결 리셋) 후 재시도 — 죽어가는 WiFi에서 낭비 구간. 모드 전환 히스테리시스 필요 근거 |
| 3 | 프록시 직행 | 10.5s | body 20209B |
| 4 | 프록시 직행 | 11.0s | body 20352B |
| 5 | 프록시 직행 | 10.1s | body 20519B |

프록시 정착 후 **~10.5s/왕복**. 내역(추정): 20KB SPI 청크 전송(15ms×10 + 오버헤드
~1.5s) + Pico DNS+TLS 풀 핸드셰이크 + Groq 추론. WiFi 직결 3.1s 대비 +7.4s —
분해하려면 구간 타임스탬프 필요(P-트랙 착수 시 추가). P-1(TLS warm)/P-3(DNS 캐시)
효과 예상 지점.

**req=2의 40s가 최우선 개선 대상**: WiFi가 실질 불능인데 esp_http_client 25s+
타임아웃을 다 기다린 후에야 프록시 재시도. 프록시 모드 전환 조건/타임아웃 단축 검토.

---

## 2026-07-13 (S-트랙 13차) — 결함 2건 추가 (부팅 브릭 + 미설정 크래시)

**결함 A — /fatfs 무한 증가로 부팅 브릭.**
테스트 대화 누적 후 재부팅 시 `/fatfs total=532480 used=532480`(100%) →
`mkdir /fatfs/memory|router_rules|scheduler` 실패 → `app_claw_start(290) 세션 매니저
설정 실패` → `ESP_ERROR_CHECK` abort → 부팅루프. 첫 런은 여유 있었으나 세션 히스토리+
메모리 파일이 storage 파티션(0x773000 len 0x8d000)을 채움. `erase_region` 후 used=180224로
회복 → 누적 데이터가 원인 확정. **미해결(완화만)**: retention/GC 정책 필요. storage 파티션
증설 또는 세션/메모리 상한+청소. 제품화 필수 항목.

**결함 B — 미설정 상태 SPI LLM_REQ → NULL mutex assert 크래시. (수정함)**
erase로 LLM 설정(backend/base_url/model/key) 소거 → `app_claw: will start without
claw_core` → agent manager 미초기화(`s_mgr.mutex` NULL). 이 상태에서 Pico가 LLM_REQ(0x05)
보내면 `on_spi_rx → claw_agent_mgr_submit_root_text → claw_agent_mgr_lock →
xQueueTakeMutexRecursive(NULL)` assert → abort 루프.
- 수정1 `claw_agent_mgr.c submit_root_text`: 락 전 `!s_mgr.initialized||!s_mgr.mutex`면
  `ESP_ERR_INVALID_STATE` 반환 (크래시 대신 정상 에러). 업스트림 가치 있는 방어.
- 수정2 `main.c on_spi_rx`: submit 실패 시 즉시 `LLM_RESP ok=false` 전송 → Pico가 90s
  타임아웃 안 기다리고 즉시 폴백.

원래 쫓던 "릴레이 정답 미전달"(12차)은 claw_core 복구(LLM 재설정) 후 재개.

---

## 2026-07-13 (S-트랙 12차) — 릴레이 정답 미전달 결함 발견

**S-트랙 시나리오 2 첫 런에서 기능 결함 포착** (S_TRACK_TEST.md 절차).

**증상**: WiFi 없이 부팅 → TG "안녕" → ESP 에이전트가 **정답 완성**
(`completion request=1 status=done raw=안녕하세요! 저는 ESP-Claw...`), 프록시 경유
Groq 200 2회 성공. 그러나 **Pico는 LLM_RESP(0x43)를 수신 못 함** → 90s 타임아웃 →
로컬 Groq 폴백 → 401(로컬 API 키 미설정) → 유저에게 "죄송합니다, 오류" 전송.
즉 정답이 있는데 못 돌려주고 망가진 폴백으로 에러 응답.

**계측 추가** (`main.c spi_llm_resp_task`):
- `receive_root_for` 반환 직후 `err` + `resp.text` 길이 로그
- LLM_RESP 전송 직후 `req/ok/slen` 로그

**배제된 가설**:
- SPI 송신 충돌: `spi_wiz_send`는 `tx_mutex`로 직렬화, rx_task도 동일 mutex → 충돌 아님.
- 경쟁 소비자 절도: 정상 채널 경로(event_router)는 `SKIP_RESPONSE_QUEUE`로 제출 →
  `response_queue`를 안 씀. match_any 소비자 없음.
- master→slave 전송 불가: 0x06/0x08/0x44 모두 Pico 정상 수신 → 전송로 정상.

**남은 두 갈래** (다음 런 로그로 판별):
1. `receive err=ESP_OK text_len=N` + `LLM_RESP sent` 로그가 뜨는데도 Pico 미수신
   → SPI 전송 타이밍 문제(Core0가 프록시 blocking 중 상태 등). 
2. `receive err=ESP_ERR_TIMEOUT text_len=-1` → 완성 응답이 `response_queue`로 안 들어감
   /엉뚱한 request_id. claw_core 라우팅 문제. 완성은 됐는데 큐 push 실패거나 id 불일치.

설계상 flags=0 제출 → 완성 시 `claw_core_agent_loop.c:477` push → `receive_for(id)`가
꺼내야 정상. 실제 안 됨 → 다음 런에서 위 로그로 확정.

**별건**: 로컬 Groq 폴백이 401 — 로컬 `llm_api_key` 미설정. 릴레이 성공 시엔 무관하나,
릴레이 실패 대비 폴백이 무의미한 상태. 웹 대시보드에서 키 설정하거나 폴백 정책 재검토 필요.

## 2026-06-23 (11차)

### Pico TLS handshake/write 무한 루프 수정 + relay timeout 원인 분석

**테스트 결과** (새 빌드 — mutex 적용):
- req=1-7 WiFi: 크래시 없음! `s_llm_http_mutex` 수정 확인
- req=7: WiFi 단절 → async extract (body=3535) + claw_core (body=21306) 순차 proxy 성공
- req=8-10 WiFi 단절: 모두 proxy 200 성공

**req=10 relay timeout 원인**:
Pico Core0 relay wait loop 내에서 `wiz_claw_http_post_cb` (Groq HTTP) blocking 호출.
Groq 응답 이후 loop 상단 deadline 체크 시 90s 초과 → timeout 출력.
"[proxy] response sent" 직후 "[relay] timeout" 인접 출력 → HTTP call이 90s에 근접했음을 의미.
**stale semaphore**: 문제 없음. `g_llm_resp_ready = false`로 각 relay 시작 시 초기화됨.

**수정** (`port/wiz-claw/src/wiz_claw_tls.c`):

1. TLS handshake busy-loop에 timeout 추가:
```c
// 이전: WANT_READ/WANT_WRITE 시 무한 busy-loop
do { ret = mbedtls_ssl_handshake(); } while (WANT_READ || WANT_WRITE);

// 이후: 5ms sleep + timeout 체크
uint32_t hs_start = to_ms_since_boot();
do {
    ret = mbedtls_ssl_handshake();
    if (WANT_READ || WANT_WRITE) {
        if (elapsed >= timeout_ms) { ret = TIMEOUT; break; }
        sleep_ms(5);
    }
} while (WANT_READ || WANT_WRITE);
```

2. `wiz_claw_tls_write` WANT_WRITE 루프에 30s deadline 추가:
```c
// 이전: WANT_WRITE 시 무한 continue
// 이후: 30s timeout → MBEDTLS_ERR_SSL_TIMEOUT 반환
```

---

## 2026-06-23 (10차)

### 동시 TLS 핸드셰이크 → LoadProhibited 크래시 수정

**증상**: req=2에서 `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)` → 재부팅.
Pico는 90s relay timeout으로 local Groq fallback 사용.

**크래시 위치**: `mbedtls_mpi_core_cond_assign` (EXCVADDR=0x00000000) ← ECC alloc OOM → NULL deref.

**스택 추적**:
```
claw_memory_async_extract_task
→ claw_llm_http_post_json → esp_http_client_perform → ssl_connect
→ mbedtls_ssl_handshake → ecp_mul_comb → LoadProhibited
```

**근본 원인**: req=2 claw_core LLM HTTP + async_extract_task LLM HTTP **동시** TLS 핸드셰이크
→ mbedtls ECC 연산에 필요한 힙 소진 → `malloc()` NULL 반환 → deref crash.

**수정**: `claw_llm_http_transport.c`에 `s_llm_http_mutex` (FreeRTOS Mutex) 추가.
WiFi TLS 경로 진입 시 acquire (`portMAX_DELAY`), `cleanup:` + inline proxy retry returns에서 release.
- claw_core: LLM 응답 받을 때까지 mutex 유지
- async extract: claw_core 완료 후 순차 실행 → 동시 TLS 불가
- mutex 초기화: `claw_llm_http_set_spi_proxy()` 또는 `claw_llm_http_set_proxy_mode()` (둘 다 LLM call 전 app_main에서 호출)

---

## 2026-06-23 (9차)

### req=4,7,9 ESP_FAIL 근본 원인 분석 및 수정

**근본 원인**: 힙 단편화 → `cJSON_Parse` OOM → `append_message_array_json` `ESP_FAIL` 반환.

상세 흐름:
1. proxy 1회당 20KB+ `sanitized_body` → `calloc` (내부 RAM)
2. 2회 연속 alloc/free → 내부 RAM 단편화 잔류
3. req=4,7,9: `claw_core_build_iteration_context`에서 Session History 적용 시
   `cJSON_Parse(session_content)` OOM → NULL 반환 → `ESP_FAIL`
4. REQUEST_START_ONLY 경로에 에러 로그 없음 → **silent failure**
5. req fail 자체는 20KB 미할당 → 힙 부분 회복 → req=5,8,10 성공

**수정 1**: `claw_core_context.c` — REQUEST_START_ONLY 경로 에러 로그 추가:
```c
// apply cached context failed request=4 provider=Session History err=ESP_FAIL
ESP_LOGW(TAG, "apply cached context failed request=%" PRIu32
         " provider=%s err=%s", ...);
```

**수정 2**: `claw_llm_http_transport.c` — `sanitized_body` PSRAM 할당:
```c
// 이전: calloc(1, len + 1)  → 내부 RAM 20KB
// 이후: heap_caps_calloc(MALLOC_CAP_SPIRAM) → PSRAM, fallback to internal
sanitized = heap_caps_calloc(1, len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!sanitized) {
    sanitized = calloc(1, len + 1);
}
```

PSRAM 할당으로 내부 RAM 단편화 제거 → 이후 `cJSON_Parse` 안정 동작 예상.

---

## 2026-06-23 (8차)

### WiFi 단절 모드 SPI proxy 안정화 테스트

**테스트 시나리오**: 부팅부터 WiFi 없이 텔레그램 대화 → 중간에 WiFi 연결.

**결과**:
- req=1: proxy 20s 타임아웃 초과 (Pico TLS+HTTP ~21s) → ESP32만 포기, Pico는 200 성공
- req=2~3, 5~6, 8, 10: proxy 정상 (200, 600~700 bytes)
- req=4, 7, 9: `ESP_FAIL` 즉시 실패 (Skills List 미로드) — 원인 조사 중
- `claw_memory`: `async extract: SPI proxy active, skipping` → 동시 proxy 충돌 없음
- 로컬 Groq fallback: `/openai/v1/chat/completions` 정확히 POST → 200 성공

**수정 (이번 세션)**:

1. `main/main.c` — proxy sem 타임아웃 20s → 45s:
```c
// 이전: 20s → Pico TLS+HTTP(20KB+) 초과 시 spurious timeout
if (xSemaphoreTake(s_http_proxy_sem, pdMS_TO_TICKS(20000)) != pdTRUE)

// 이후: 45s
if (xSemaphoreTake(s_http_proxy_sem, pdMS_TO_TICKS(45000)) != pdTRUE)
```

2. `main/main.c` — proxy lock 타임아웃 0 → 30s:
```c
// 이전: 0ms (concurrent call 즉시 ESP_ERR_INVALID_STATE)
if (xSemaphoreTake(s_spi_proxy_lock, 0) != pdTRUE)

// 이후: 30s (대기 후 순차 진행)
if (xSemaphoreTake(s_spi_proxy_lock, pdMS_TO_TICKS(30000)) != pdTRUE)
```

**미해결**:
- req=4,7,9 `ESP_FAIL` (2ms 내 실패, HTTP 없이) — 힙 단편화 또는 claw_core 내부 조건 가능성
- WiFi 재연결 직후 relay timeout (과도기적 현상, 이후 정상)

---

## 2026-06-23 (7차)

### `spi_llm_resp_task` 조기 timeout → ok=false 버그

**증상**: ESP32 `completion status=done raw=안녕 테오!` 찍히는데 Pico에서 `LLM_RESP error` 수신 → `죄송합니다` 전송.

**원인**: `spi_llm_resp_task`가 `claw_agent_mgr_receive_root_for(..., 30000)` — 30초 대기.

WiFi TLS 실패 시 타임라인:
```
시도 1: 최대 25s 타임아웃
재시도 딜레이: ~3-5s
시도 2: ~10s 성공
합계: ~38s  >  30s 제한
```

→ `spi_llm_resp_task` 30s에 타임아웃 → `ok=false` LLM_RESP 전송 → Pico 즉시 fallback.
→ 실제 LLM은 8s 후 성공 → `status=done raw=안녕 테오!` — 이미 늦음.

**픽스**: `main/main.c` timeout 30000 → 85000ms.

```c
// 변경 전
esp_err_t err = claw_agent_mgr_receive_root_for(a->request_id, &resp, 30000);

// 변경 후
// Worst case: 25s WiFi + 5s delay + 25s retry + 20s SPI proxy = ~75s → 85s로 여유
esp_err_t err = claw_agent_mgr_receive_root_for(a->request_id, &resp, 85000);
```

**파일**: `application/edge_agent/main/main.c`

---

## 2026-06-23 (6차)

### Pico relay — ESP32 에러 응답 즉시 처리

**문제**: ESP32 context 빌드 실패 시 `LLM_RESP ok=false` 전송. Pico relay 루프가 이를 무시 → 90초 전부 대기 → 타임아웃 → 로컬 Groq fallback (401).

**원인**: `on_spi_rx`에서 `ok=false`일 때 `g_llm_resp_ready` 세우지 않음 → Core 0 루프 탈출 불가.

**픽스**: `g_llm_resp_ok` 플래그 추가.

```c
// on_spi_rx — ok=false일 때도 ready 세움
g_llm_resp_ok    = false;  // 에러 표시
g_llm_resp_ready = true;   // 즉시 루프 탈출

// relay 루프 — ok/error 구분
if (got && g_llm_resp_ok) {
    send_text = g_llm_resp_text;   // 정상: Telegram 전송
} else if (got) {
    // 에러: 즉시 로컬 Groq fallback (90초 낭비 없음)
} else {
    // 타임아웃: 로컬 Groq fallback
}
```

**효과**: ESP32 에러 → Pico 즉시 fallback. 90초 → ~0ms 단축.

**파일**: `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host\main.c`

---

### req=3 ESP_FAIL 분석 (일시적 힙 단편화)

req=2 (body 20864 bytes) 프록시 후 힙 조각 발생. req=3 context 빌드 시 `cJSON_CreateArray` 또는 Skills List 할당 실패 → ESP_FAIL. 110초 후 req=4 정상 (session history 더 큰데도 성공) → 자가 회복. 재시도 로직 불필요.

---

## 2026-06-23 (5차)

### SPI 프록시 전체 검증 완료 ✅

두 시나리오 모두 연속 성공 확인.

#### 시나리오 A: WiFi 후 안테나 뽑기

- ESP32 SPI proxy 모드 진입 후 req 1~5 연속 성공
- 각 body 20289~20623 bytes 정상 전달 (`BODY_END rcv=20289/20289` 등)
- CRC 에러 없음, cmd=0xF0 없음

```
[proxy] BODY_END rcv=20289/20289
[proxy] response sent status=200 body=678 bytes
[relay] ESP32 CLAW response received
[reply] ㅎㅎ 안녕! 즐거운 시간 보내고 계세요!
```

#### 시나리오 B: 콜드부트 (안테나 없음)

- 부팅 직후 WiFi DNS 실패 → SPI proxy 전환 ~2초
- req 1: body=20746 bytes → status=200 ✅
- req 2: body=20864 bytes → status=200 ✅

```
W (47008) llm_http: HTTP failed, retrying via SPI proxy inline
I (47009) spi_proxy: HTTP_REQ url=https://api.groq.com/...  body=20746 bytes
I (56492) spi_proxy: response status=200 len=634
```

**관찰**: `proxy busy (concurrent call)` 경고 1회 발생 — memory async extract가 main LLM proxy와 동시 시도. 비치명적. 이후 요청부터 `async extract: SPI proxy active, skipping`으로 정상 우회.

---

### 확정된 픽스 목록 (전체)

| 픽스 | 파일 | 효과 |
|------|------|------|
| tx_mutex in s_rx_task | `spi_wiz.c` | SPI assert 크래시 제거 |
| drain-before-send | `main/main.c` | 13초 딜레이 제거 |
| CRC err defer to BODY_END | `main.c` (Pico) | Bad magic 0x7B 0x22 제거 |
| relay timeout 35s→90s | `main.c` (Pico) | 타임아웃 미스매치 해결 |
| LLM HTTP timeout 120s→25s | `claw_llm_runtime.c` | WiFi 실패 감지 가속 |
| g_esp32_alive 유지 | `main.c` (Pico) | 재연결 자동 복구 |
| MAX_CHUNK 4096→2048 | 양쪽 proto header | CRC 에러율 감소 |
| 청크 간 딜레이 5ms→15ms | `main/main.c` (ESP32) | Pico 처리 여유 확보 |
| HTTP_BODY printf 억제 | `wiz_spi_slave.c` | FIFO overflow → cmd=0xF0 방지 |
| HTTP_REQ 후 20ms 딜레이 | `main/main.c` (ESP32) | Pico malloc 완료 전 BODY 도착 방지 |

---

### 미해결 이슈

| 이슈 | 상태 | 메모 |
|------|------|------|
| LLM_RESP 순서 역전 (req=1에서 가끔) | 낮은 우선순위 | 치명적이지 않음, 재현 드묾 |
| memory async extract SPI proxy 중 skip | 설계 한계 | proxy 점유 중 extract 불가 — 허용 가능 |
| 시간 동기화 실패 (WiFi 없음) | 기대 동작 | `cap_time: Time sync failed` — NTP는 WiFi 필요 |

---

## 2026-06-23 (4차)

### `cmd=0xF0` SPI 스트림 동기화 버그 근본 원인 분석 + 픽스

#### 근본 원인

`wiz_spi_slave.c` 코드 분석 결과 두 가지 원인 확인:

**원인 A: HTTP_BODY 수신 중 `printf` → FIFO overflow**

```c
// 변경 전 (_handle_rx의 로그 조건)
if (cmd != SPI_CMD_CHUNK_DATA) {
    printf("[spi_slave] RX cmd=0x%02X seq=%u len=%u\n", ...);
}
```

CHUNK_DATA는 이미 printf 억제 중. HTTP_BODY(cmd=0x07) 수신 직후 printf ~3ms 실행 → PL022 SPI FIFO 8 bytes 오버플로 → 다음 청크 앞 바이트 드롭 → 슬라이딩 윈도우가 잘못된 바이트를 MAGIC_0으로 인식 → cmd byte misread.

**원인 B: HTTP_REQ 직후 첫 BODY 딜레이 없음**

ESP32가 HTTP_REQ 전송 직후 즉시 첫 BODY 청크 전송. Pico Core 1은 HTTP_REQ JSON 파싱 + `malloc(body_len+1)` 실행 후 Core 1 루프로 돌아오기 전에 BODY 바이트가 이미 도착 → FIFO 압박.

#### 픽스

**픽스 1 — Pico `wiz_spi_slave.c`: HTTP_BODY printf 억제**

```c
// 변경 전
if (cmd != SPI_CMD_CHUNK_DATA) {

// 변경 후
if (cmd != SPI_CMD_CHUNK_DATA && cmd != SPI_CMD_HTTP_BODY) {
```

HTTP_BODY 청크 수신 중 시리얼 출력 차단. CHUNK_DATA와 동일 패턴.

**픽스 2 — ESP32 `main/main.c`: HTTP_REQ 후 20ms 딜레이 추가**

```c
// HTTP_REQ 전송 후, 첫 BODY 청크 전에
vTaskDelay(pdMS_TO_TICKS(20));  /* Pico needs time to parse HTTP_REQ JSON + malloc body buf */
```

Pico가 malloc + JSON 파싱 완료 후 Core 1 poll 루프로 복귀할 시간 확보.

---

### 수정한 파일

| 파일 | 변경 내용 |
|------|-----------|
| `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host\wiz_spi_slave.c` | HTTP_BODY printf 억제 |
| `application/edge_agent/main/main.c` | HTTP_REQ 후 20ms 딜레이 |

---

### 이슈 / 미해결 (업데이트)

| 이슈 | 상태 | 메모 |
|------|------|------|
| `cmd=0xF0` 동기화 버그 | **픽스 적용** | printf 억제 + REQ 후 딜레이. 재테스트 필요 |
| CRC 에러 완화 검증 | 미완료 | 2048 청크 + 15ms + 20ms 딜레이로 재테스트 |
| LLM_RESP 순서 역전 | 미해결 | claw_core 코드 분석 필요 |
| Pico Groq API 키 미설정 | 미해결 | `http://192.168.11.20/` 웹 대시보드 설정 |

---

## 2026-06-22 (3차)

### WiFi 끊김 SPI 프록시 재테스트 실패 분석

세 가지 문제 발견:

#### 문제 1: SPI CRC 에러 (4096-byte 청크)

```
[spi_slave] CRC error: got 0x3A, expected 0x37
hdr: CA FE 07 00 10 08 3A   (cmd=HTTP_BODY, len=4096)
```

첫 번째 4096-byte 청크에서 페이로드 비트 에러. 이후 청크들은 정상. 청크 크기를 줄여 청크당 에러 확률 감소.

**픽스**: `SPI_CLAW_MAX_CHUNK` 4096→2048 (ESP32 + Pico 양쪽 proto header 모두)

#### 문제 2: CRC 에러 후 `cmd=0xF0` — SPI 스트림 동기화 깨짐

CRC 에러 → 500 응답 후, 다음 HTTP_REQ의 BODY 청크가 `cmd=0xF0`으로 수신됨:
```
[spi_slave] RX cmd=0xF0 seq=68 len=3490
[spi_rx] unhandled cmd=0xF0 seq=68 len=3490
```

CRC 에러 복구 과정에서 SPI slave 상태가 깨진 것으로 추정. 청크 크기 축소 + 청크 간 딜레이 증가로 완화 시도.

**픽스**: ESP32 청크 간 딜레이 5ms→15ms (`spi_http_proxy_fn`)

#### 문제 3: LLM_RESP(error) → HTTP_REQ 순서 역전

```
seq=6: LLM_RESP ok=false  ← 에러 응답이 먼저 도착
seq=7: HTTP_REQ            ← 프록시 요청이 나중에 도착
```

ESP32 claw_core가 프록시 결과 확인 전에 에러 LLM_RESP를 먼저 전송하는 것으로 추정. 현재 Pico relay 루프는 `ok=false` LLM_RESP를 무시하고 대기 유지하므로 치명적이지 않으나 근본 원인은 미파악.

**미해결**: claw_core LLM_RESP 전송 시점 확인 필요.

---

### 수정한 것

#### 1. `SPI_CLAW_MAX_CHUNK` 4096→2048 (양쪽 모두)

```c
// ESP32: application/edge_agent/components/spi_wiz/include/wiz_claw_spi_proto.h
// Pico:  examples/wiz_claw_spi_host/wiz_claw_spi_proto.h
#define SPI_CLAW_MAX_CHUNK 2048u
```

20KB body: 5청크→10청크. 청크당 바이트 절반 → 청크당 비트 에러 확률 절반.

#### 2. ESP32 청크 간 딜레이 5ms→15ms

```c
// main/main.c — spi_http_proxy_fn 청크 전송 루프
vTaskDelay(pdMS_TO_TICKS(15));  // 이전: 5ms
```

Pico SPI slave 처리 여유 시간 확보.

---

### 이슈 / 미해결

| 이슈 | 상태 | 메모 |
|------|------|------|
| CRC 에러 완화 검증 | 미완료 | 2048 청크 + 15ms 딜레이로 재테스트 필요 |
| `cmd=0xF0` 동기화 버그 | 미해결 | CRC 에러 후 SPI slave 상태 리셋 로직 검토 필요 |
| LLM_RESP 순서 역전 | 미해결 | claw_core 코드 분석 필요 |
| Pico Groq API 키 미설정 | 미해결 | `http://192.168.11.20/` 웹 대시보드에서 설정 |

---

## 2026-06-22 (2차)

### 엔드-투-엔드 검증 완료

이전 세션의 픽스 3개(mutex, drain-before-send, CRC defer) 플래시 후 첫 요청 성공:

```
Telegram "안녕" → Pico SPI LLM_REQ → ESP32 completion → LLM_RESP ok
→ Pico TLS → Telegram sendMessage 200 → 답장 수신
```

CRC 에러 없음. 13초 딜레이 없음.

---

### WiFi 끊김 시 폴백 실패 분석

안테나 제거 후 두 번째 요청 로그 분석 결과 타이밍 불일치 발견:

**타임라인:**
```
748,103ms  Pico → ESP32: LLM_REQ
           ESP32: WiFi → api.groq.com TLS 시도
798,144ms  ESP32: mbedtls SSL handshake 실패 (2730ms 대기 후 재시도)
819,262ms  ESP32: 2차 시도 실패 → SPI proxy 전환  ← 71초 경과
```

Pico relay timeout = 35s → 이미 만료 → 로컬 Groq fallback 실행 → 401 (API 키 없음) → 오류 답장.

ESP32가 SPI proxy HTTP_REQ 날릴 때 Pico는 이미 딴 경로 처리 중.

---

### 수정한 것

#### 1. Pico `main.c` — relay timeout 35s → 90s

**파일:** `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host\main.c`

```c
// 수정 전
#define LLM_RELAY_TIMEOUT_MS 35000u

// 수정 후
#define LLM_RELAY_TIMEOUT_MS 90000u
```

ESP32 WiFi 재시도 + SPI proxy 전환 총 소요 ~71s → 90s로 커버.

---

#### 2. Pico `main.c` — relay timeout 시 ESP32 비활성화 제거

relay 타임아웃이 발생해도 `g_esp32_alive = false`로 ESP32를 죽은 것으로 처리하지 않음.
ESP32는 주기적으로 `SPI_CMD_ESP_STATUS`를 전송하므로 다음 패킷 수신 시 자동 복구됨.

```c
// 수정 전
printf("[relay] timeout — falling back to local Groq\n");
g_esp32_alive = false;  /* 이번 세션 동안 로컬로 전환 */

// 수정 후
printf("[relay] timeout — falling back to local Groq\n");
/* g_esp32_alive 유지: ESP32 STATUS 패킷 오면 자동 복구 */
```

---

#### 3. ESP32 `claw_llm_runtime.c` — LLM HTTP timeout 120s → 25s

**파일:** `d:\ESP-IDF\esp-claw\components\claw_modules\claw_core\src\llm\claw_llm_runtime.c`

```c
// 수정 전
#define CLAW_LLM_DEFAULT_TIMEOUT_MS (120 * 1000)

// 수정 후
#define CLAW_LLM_DEFAULT_TIMEOUT_MS (25 * 1000)
```

WiFi 없을 때 TLS 실패를 25s 내에 감지 → SPI proxy 전환. Pico relay 90s 기준 여유 있음.
정상 WiFi 환경에서 Groq 응답은 통상 5s 이내 → 25s로 충분.

---

### 이슈 / 미해결

| 이슈 | 상태 | 메모 |
|------|------|------|
| Pico 로컬 Groq API 키 미설정 | 미해결 | `http://192.168.11.20/` 웹 대시보드에서 `llm_api_key` 입력 필요. 설정 안 되면 fallback 시 401 |
| WiFi 폴백 픽스 검증 | 미완료 | 안테나 제거 후 재테스트 필요 |
| SPI 클럭 2MHz | 유지 | CRC 에러 미발생 중, 문제 없으면 현행 유지 |

---

## 2026-06-22

### 구현한 것

#### 1. `spi_wiz.c` — `s_rx_task` mutex 추가 (크래시 픽스)

**증상**: 첫 부팅 시 `assert failed: spi_device_transmit spi_master.c:1310 (ret_trans == trans_desc)` 크래시.

**원인**: `s_rx_task`가 `tx_mutex` 없이 `spi_device_transmit`을 호출하고 있었음. `spi_wiz_send()`는 `tx_mutex`를 잡고 호출하지만 `s_rx_task`는 무방비 상태 → 두 태스크가 동일 SPI device handle에 동시 접근 → IDF 내부 assert 발생.

**픽스**: `s_rx_task`에서 헤더 읽기 시작 전 `xSemaphoreTake(h->tx_mutex, portMAX_DELAY)`, 페이로드 읽기 완료 후 `xSemaphoreGive`. 중간 에러(bad magic, OOM, 페이로드 RX 실패) 경로 전부 mutex release 추가.

```c
// spi_wiz.c: s_rx_task 내부
xSemaphoreTake(h->tx_mutex, portMAX_DELAY);
// ... header RX → magic check → payload RX ...
xSemaphoreGive(h->tx_mutex);
// CRC 검증 및 on_rx() 콜백은 mutex 해제 후
```

---

#### 2. `main/main.c` — drain-before-send 픽스 (13초 딜레이)

**증상**: SPI 프록시로 LLM 요청 시 응답이 13~20초 지연.

**원인**: `spi_http_proxy_fn`에서 응답 버퍼 초기화 + 세마포어 drain을 `HTTP_BODY_END` 전송 **이후**에 수행. Pico에서 CRC 에러 발생 시 바디 전송 중간에 500 응답이 도착하지만 BODY_END 후 reset이 해당 응답을 지워버림 → `xSemaphoreTake(s_http_proxy_sem, 20s)` 20초 대기 시작 → `spi_status_task`(20초 주기)가 제공하는 SPI 클럭에 Pico의 block 해제 → 약 13초 후 unblock.

**픽스**: reset + drain을 `HTTP_REQ` 전송 **이전**으로 이동. 이전 요청의 잔여 응답만 drain되고 현재 요청의 응답은 보호됨.

```c
// spi_http_proxy_fn: HTTP_REQ 전송 전
free(s_http_proxy_resp_body);
s_http_proxy_resp_body   = NULL;
s_http_proxy_resp_cap    = 0;
s_http_proxy_resp_len    = 0;
s_http_proxy_resp_status = 0;
xSemaphoreTake(s_http_proxy_sem, 0);  // stale drain

// 이후 HTTP_REQ → BODY chunks → BODY_END 전송
// BODY_END 이후에는 reset 없음
```

---

#### 3. Pico `main.c` — CRC 에러 응답 defer (Bad magic 픽스)

**증상**: `[spi_wiz] Bad magic: 0x7B 0x22` + `response status=0` → 프록시 실패.

**원인**:
- Pico CRC 에러 콜백이 즉시 `_proxy_send_response(0, NULL)` 호출 (HTTP_RESP 7-byte 헤더 전송)
- ESP32가 바디 청크(4103 bytes) `spi_device_transmit` 중에 Pico가 `spi_write_blocking`으로 헤더를 슬레이브 TX 시작
- ESP32 `spi_wiz_send`가 `tx_mutex`를 먼저 획득 → `spi_device_transmit` 진행 (rx_buffer=NULL) → Pico 헤더 7바이트가 청크의 첫 7 클럭에 소비되어 **묵살**
- 이후 `s_rx_task`가 mutex 획득 시점에 HTTP_RESP JSON 페이로드(`{"statu...` = `0x7B 0x22`)를 읽음 → Bad magic
- HTTP_RESP_END 도착 → 세마포어 given → status 여전히 0

**픽스**: Pico `on_spi_crc_error()` 콜백에서 즉각 응답 제거. `g_proxy_crc_err = true` 플래그 set. `SPI_CMD_HTTP_BODY_END` 핸들러에서 플래그 확인 후 500 전송. BODY_END 시점은 ESP32가 TX를 완료한 후라 `s_rx_task`가 idle 상태 → 레이스 없음.

```c
// Pico main.c
static bool g_proxy_crc_err = false;

static void on_spi_crc_error(uint8_t raw_cmd, void *user_ctx) {
    if (raw_cmd == SPI_CMD_HTTP_BODY || raw_cmd == SPI_CMD_HTTP_BODY_END ||
        raw_cmd == SPI_CMD_HTTP_REQ) {
        // free body, reset state
        g_proxy_crc_err = true;  // 즉시 응답 대신 플래그
    }
}

// BODY_END 핸들러
case SPI_CMD_HTTP_BODY_END:
    if (g_proxy_crc_err) {
        _proxy_send_response(0, NULL);  // 이제 s_rx_task idle → 안전
        g_proxy_crc_err = false;
    }
    // ...
```

---

#### 4. `CLAUDE.md` 작성

레포 루트의 `CLAUDE.md`를 단순 `AGENTS.md` 참조에서 실질적인 내용으로 교체. 빌드 환경 제약(Claude Code 셸에서 idf.py 직접 실행 불가), 레포 레이아웃, SPI 브릿지 아키텍처, Pico 사이드 프로젝트 위치 포함.

---

### 이슈 / 미해결

| 이슈 | 상태 | 메모 |
|------|------|------|
| 4096-byte 청크 CRC 에러 | 미해결 | 신호 품질 문제 추정. SPI 클럭 8MHz→4MHz 또는 `SPI_CLAW_MAX_CHUNK` 축소 고려 |
| Pico 로컬 Groq API 키 미설정 | 미해결 | Pico 설정 파일에 Groq API 키 없음 → 폴백 시 401 |
| 엔드-투-엔드 검증 | 미완료 | 세 가지 픽스 플래시 후 정상 흐름(HTTP 성공 → LLM_RESP ok=true) 미확인 |

---

### 다음 할 일

1. **빌드 & 플래시**: ESP32 (이번 세션 픽스 3개 반영) + Pico (`g_proxy_crc_err` 로직) 동시 플래시
2. **CRC 에러 완화**: `SPI_WIZ_CLOCK_HZ` 를 `2000000` (현재 Kconfig 기본값) 유지 또는 더 낮춤. 아니면 `SPI_CLAW_MAX_CHUNK`를 2048로 줄여 청크당 에러율 감소
3. **ESP32 프록시 재시도**: 500 응답 수신 시 1회 재시도 로직 추가 고려 (`spi_http_proxy_fn` 내부)
4. **Pico Groq 키 설정**: Pico 측 설정에 `GROQ_API_KEY` 입력
5. **정상 경로 검증**: `안녕` 메시지 → Telegram → Pico LLM_REQ → ESP32 에이전트 → Groq → LLM_RESP(ok=true) → Telegram 답장 전체 흐름 확인
