# S-트랙 안정성 검증 프로토콜

> TASK_BRIEF_WIRED_V2.md S-트랙 실행 문서. **수정/튜닝 전 베이스라인 확보가 목적.**
> 여기서 나온 수치가 P-트랙(성능)·E-트랙(구조) 개선의 before/after 기준이 된다.
> 작성 2026-07-13. 대상: ESP `hybrid/w55rp20` + Pico `main` (스냅샷 커밋 이후).

---

## 0. 준비 (S-1 계측 — 이미 코드에 반영됨)

이번 커밋에서 추가된 계측:

**Pico** (`examples/wiz_claw_spi_host/main.c`):
- `[health]` 라인 30초 주기 출력: `up=<uptime_s> heap=<free_bytes> esp_alive=<0|1> relay_ok=N relay_err=N relay_to=N local_fb=N`
- 릴레이 결과 카운터: ok(정상 릴레이) / err(ESP ok=false) / to(90s 타임아웃) / local_fb(로컬 Groq 폴백)

**ESP** (`application/edge_agent/main/main.c`, `spi_status_task`):
- `[health] free_heap=N min_ever=N` 20초 주기 (ESP_LOGI)

### 로그 수집
- Pico: USB CDC 시리얼 (Tera Term / `idf.py` 아님 — pico는 별도). 921600 또는 기본 보레이트. 파일로 캡처.
- ESP: `idf.py -p COM<x> monitor` 또는 시리얼 캡처. 별도 터미널.
- **두 로그에 타임스탬프 붙여 저장** (Tera Term: Timestamp 옵션 ON). 나중에 상호 대조용.

### 빌드/플래시 (사용자 실행 — Claude가 못 함)
```
# ESP (IDF export된 셸에서)
cd application/edge_agent
idf.py build && idf.py -p COM<esp> flash monitor

# Pico
cd examples/wiz_claw_spi_host && (build 디렉토리에서) cmake --build .
# .uf2 → BOOTSEL 드래그
```

---

## 1. 시나리오 매트릭스 (각 20회 이상 반복)

각 시나리오: 시작 시각 기록 → 실행 → `[health]` 라인 + 관련 로그 캡처 → 아래 표에 결과 기입.

| # | 시나리오 | 절차 | 확인 항목 (PASS 조건) |
|---|---------|------|----------------------|
| 1 | WiFi ON + 유선 연결 | ESP WiFi 정상 상태에서 TG 대화 20회 | 무선 직결 경로 사용(relay 미사용 or 정상). 회귀 없음. ESP `min_ever` 급락 없음 |
| 2 | WiFi OFF 부팅 → TG 대화 | ESP를 WiFi 없이 부팅, TG 20회 | `relay_ok` 증가, `relay_to`/`relay_err` 최소. 왕복 시간 기록(로그 타임스탬프 차) |
| 3 | 대화 중 WiFi 절단/복구 | 대화 중 AP 껐다 켬 3회 | proxy 모드 전환 정상, 크래시 0. WiFi 복구 후 직결 복귀 |
| 4 | ESP 리셋 (Pico 유지) | Pico 살린 채 ESP 리셋 버튼 | **핵심**: `esp_alive` 거동 관찰. 리셋 후 매 요청 `relay_to`(90s 낭비) 반복되는지 확인 → dead-ESP 미감지 결함 정량화 (P-5 근거) |
| 5 | 장문 응답 (>16KB) | 긴 답변 유도 프롬프트 | 청크 조립 정상, `HTTP_PROXY_BODY_MAX`(32KB) 경계 안전. 잘림/크래시 0 |
| 6 | 연속 요청 (프록시 직렬화) | 짧은 간격 연속 10요청 × 3세트 | `s_llm_http_mutex`/`s_spi_proxy_lock` 직렬화 정상. **힙 단조 감소 없음**(양쪽 `heap`). DEVLOG 9차 회귀 확인 |
| 7 | PIR 트리거 중 TG poll | PIR 발동 타이밍 다양화 | 알림 지연 실측(최대 ~5s poll 블로킹 + 캡처). 60s 쿨다운 동작. 크래시 0 |

### 기록 표 템플릿 (시나리오별)
```
S#: __  회차: __/20  시작: __:__:__
결과: PASS / FAIL
relay_ok/err/to/local_fb: __/__/__/__
Pico heap 시작→끝: ______ → ______
ESP free_heap/min_ever: ______ / ______
왕복 시간(로그): ___ms
비고(크래시/이상):
```

---

## 2. 알려진 취약점 회귀 확인 (S-3)

DEVLOG.md 9~11차 픽스가 유지되는지 각 1회 재현:

| DEVLOG | 증상 | 재현 시나리오 | PASS |
|--------|------|--------------|------|
| 10차 | 동시 TLS 핸드셰이크 → LoadProhibited 크래시 | WiFi 단절 + claw_core LLM + async memory extract 동시 유발 (연속 요청) | `s_llm_http_mutex`로 직렬화, 크래시 0 |
| 9차 | 힙 단편화 → cJSON_Parse OOM → ESP_FAIL (req 4/7/9) | 20KB+ 응답 연속 유발 | PSRAM 할당 유지, `min_ever` 안정, silent fail 0 |
| 11차 | Pico TLS handshake/write 무한루프 | WiFi 단절 프록시 요청 반복 | handshake/write 타임아웃 동작, hang 0 |

---

## 3. 24h Soak 테스트

**구성**: UAExpert 없음(이건 opcua_node 예제 얘기 — 여기선 해당 없음). 대신:
- WiFi 단절 모드로 고정 (프록시 경로 상시 사용)
- 스크립트/수동으로 TG 요청 주기적 주입 (예: 5분마다 1건) 또는 봇에 자동응답 루프
- 24h 방치

**PASS 조건**:
- 재부팅 0회 (Pico `up=` 단조 증가, 리셋 시 0으로 리셋되면 FAIL)
- 힙 누수 0: Pico `heap`, ESP `free_heap`/`min_ever` 시작~24h 후 비교 시 추세 하락 없음 (±노이즈 허용, 단조 감소 금지)
- `relay_to`/`relay_err` 폭증 없음 (dead-ESP 락업 아니면)
- 카운터 정상 집계 (ParseError/Timeout 등)

**수집**: `[health]`/`[health]` 라인만 grep해서 CSV로 뽑으면 추세 그래프 가능:
```
# Pico 로그에서
grep '\[health\]' pico.log > pico_health.csv
# ESP 로그에서
grep 'free_heap=' esp.log > esp_health.csv
```

---

## 4. 판정 → 다음 트랙 분기

- **전부 PASS + 힙 안정** → P-트랙(성능 튜닝) 착수. 시나리오 2/4의 왕복시간·`relay_to` 수치가 P-1(TLS warm)·P-5(dead-ESP 감지) 우선순위 근거.
- **크래시/누수 발견** → 해당 결함 먼저 수정, S-트랙 재실행. 신규 결함은 DEVLOG.md에 기록.
- 시나리오 4에서 `relay_to`가 매 요청 반복되면 → **P-5(dead-ESP 감지)가 P-트랙 최우선**으로 승격.

---

## 5. 계측이 부족하면 (추가 후보)

지금은 저위험 최소셋만 넣음. 아래는 필요 시 추가:
- 구간 타임스탬프(요청 수신→SPI 완료→DNS→TLS 완료→첫 바이트→응답 완료)를 LLM_RESP에 실어 양쪽 대조 — **P-트랙에서 TLS 핸드셰이크 비용 정밀 측정할 때** 필요. `wiz_claw_http.c`에 phase 타이밍 구조체 + getter 추가하는 작업(브리프 S-1 상세). 스테이지별 수치가 필요해지면 그때 심는다.
- ESP 태스크별 스택 하이워터마크(`uxTaskGetStackHighWaterMark`) — 스택 오버플로 의심 시.
