# TASK_BRIEF V2: esp-claw 유선 하이브리드 — 정리·안정화·업스트림 준비

> 작성일: 2026-07-13. TASK_BRIEF.md(Phase 1~3, 구버전)를 대체한다.
> 2026-07-13 코드 검증 보고서 기준으로 작성 — 아래 0장 수치는 전부 실측이다.
> 방향성 점검 추가: 2026-07-15 (아래 "★ 북극성 & U-트랙" — 매 세션 먼저 읽을 것).

---

## ★ 북극성 & U-트랙 (매 세션 먼저 읽기 — 산으로 가지 않기 위한 나침반)

**최종 목적**: espressif/esp-claw에 **WIZnet 유선 솔루션을 알리고 안착**시킨다
(공식 기능 또는 WIZnet-maintained integration). 사설 데모 완성이 목표가 아니다.

**파는 것 / 지키는 것**: 후크는 **AI 에이전트**, 차별점은 **네트워크가 안 끊김**.
한 줄 = "WiFi 죽어도, 펌웨어 죽어도, 텔레그램 속 AI가 계속 답한다."

**정체성(왜 W5500 직결이 아니라 W55RP20 코프로세서인가)**: **fault-domain 격리** —
연산(ESP32, 무겁고 크래시남) ↔ 네트워크/관리(W55RP20, 상시생존)를 분리. 이것이
W5500 단독 연결이 원천적으로 못 하는 유일한 가치. 모든 설계는 이 문장을 지지해야 함.

**upstream 전략(분리)**: PR로 올리는 것 = **벤더중립 트랜스포트 추상화**(esp_eth+W5500로도
동작). 파는 것 = 그 추상화를 가장 잘 구현하는 **WIZnet guardian 보드**. **코어에 WIZnet
보드 의존을 넣어달라고 하지 않는다**(리젝 사유). 자세한 분석·근거: `docs/DEVLOG.md` 20차.

### U-트랙 체크리스트 (업스트림 지향 — 이게 최종 목적에 직접 기여하는 유일한 트랙)
- [ ] **U-1. 차별점 MVP 완성 = EN 리셋선.** HUNG/DEAD 감지는 됨(heartbeat), **물리 리셋
      (Pico GPIO→ESP EN)이 마지막 다리.** 이거 돼야 "독립 감시자 + 구원자" 서사 완성 + 데모 가능.
- [ ] **U-2. 벤치마크표**: guardian vs (dumb 워치독IC + W5500 직결). 항목: ESP 행업 시
      네트워크 생존 / 복구시간 / OOB 관리 / 메시지 유실. "왜 RP2040?"의 유일한 정당화 문서.
- [ ] **U-3. E-1 벤더중립 추상화 PR**: `claw_llm_spi_proxy_fn_t` → 트랜스포트 중립 훅.
      작고 깨끗한 첫 PR 후보 (E-트랙 §5 참조).
- [ ] **U-4. PoC 패키지**: 블록도 + "랜선 뽑기 / ESP 죽이기 → 계속 답함" 영상 + DEVLOG 수치.
- [ ] **U-5. Feature Proposal Issue** (esp-claw): "working prototype 있음" + U-2 + U-4.
      **일찍 접촉** — 과투자 전에 maintainer와 인터페이스 위치 합의부터. (§7 로드맵과 연결)

### 방향성 점검 규칙 (드리프트 방지)
1. **매 세션, 이 U-트랙에서 뭐가 전진했나 먼저 물어라.** 내부 견고화만 반복하면 목적 이탈.
2. **robustness는 타임박스.** "튼튼한 사설 PoC"는 수단이지 목적이 아님. U-트랙이 우선.
3. **esp-claw 코어 버그(힙손상/툴검증400/세션누적)는 쫓지 않는다** — 우리 것도 기여물도
   아님. Guardian이 **감지·복구·완화하는 대상**으로 남기고, 필요 시 별도 upstream issue로
   분리 보고. (DEVLOG 20차 경계 참조)
4. 차별점(fault-domain 격리)을 **지지하지 않는** 기능(RSSI 사전전환, SSE 등)은 후순위.

---

## 0. 확정 전제 사실 (검증 완료)

### 저장소
| 역할 | 경로 | git 상태 |
|------|------|----------|
| ESP측 (호스트 MCU) | `D:\ESP-IDF\esp-claw` (app: `application\edge_agent`) | origin = espressif/esp-claw 직결(fork 아님). 로컬커밋 `4bebda7` 1개 + **HTTP 프록시 확장 미커밋**. upstream master 대비 **83커밋 뒤** |
| Pico측 (네트워크 코프로세서) | `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\examples\wiz_claw_spi_host` | 커밋 `6486955` 이후 핵심 변경 **미커밋** |
| ~~D:\PICO2\pico2_test\WIZnet-PICO-C~~ | 구버전 Phase-1 데모. **폐기 대상** — 참조 금지 |

### ESP측 훅 3지점
1. LLM 트랜스포트 분기: `components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c:330` — 함수포인터 `claw_llm_spi_proxy_fn_t`, `g_use_spi_proxy && s_spi_proxy_fn` 분기. 등록: `main.c:974 claw_llm_http_set_spi_proxy()`
2. 프록시 모드 전환: `main.c:685 on_wifi_state_changed` (wifi_manager 콜백) + WiFi 실패 시 인라인 재시도 `transport.c:443`
3. IM 인입: Pico TG 수신 → `SPI_CMD_LLM_REQ(0x05)` → ESP `main.c:519 on_spi_rx` → `claw_agent_mgr_submit_root_text()` → `SPI_CMD_LLM_RESP(0x43)`

### SPI 프로토콜 (양쪽 동일 헤더, 각자 사본)
- 7바이트 헤더: magic[2]=CA FE / cmd / len(u16 LE) / seq / crc(XOR). 버전 필드 없음
- 커맨드: 0x01~0x08 (PING, LUA_EXEC, GPIO_SET, CAPTURE_REQ, LLM_REQ, HTTP_REQ, HTTP_BODY, HTTP_BODY_END), 0x40~0x47 (EVENT, CHUNK_DATA, CHUNK_END, LLM_RESP, ESP_STATUS, HTTP_RESP, HTTP_RESP_BODY, HTTP_RESP_END), 0x80~0x82 (ACK, NACK, PONG). NACK은 정의만, 송신 코드 없음
- `SPI_CLAW_MAX_CHUNK` = **2048** (4096 아님 — wiz_spi_slave.c:116 주석이 낡은 것)
- ESP 고정 딜레이: HTTP_REQ 후 20ms, BODY 청크 간 15ms. Pico 응답 청크 간 5ms
- Pico 릴레이 대기: `sleep_ms(10)` 폴링, `LLM_RELAY_TIMEOUT_MS = 90000` (90s). ok:false 수신 시 즉시 폴백
- `g_esp32_alive`: PING/ESP_STATUS 수신 시 true, **false로 되돌리는 코드 없음** (dead-ESP 미감지)
- ESP 송신: 첫 PONG까지 3s PING + 20s 주기 ESP_STATUS(하드코딩 `{"wifi":true,"agent":true}`)

### 성능 병목 (확정 진단)
유선 경로 지연 주범 = **Pico가 요청마다 수행하는 TLS 풀 핸드셰이크** (`do_http_request`: 매 요청 DNS resolve + TLS init/connect/close, `Connection: close`, 세션 재개 없음). SPI 청크/딜레이는 수백 ms 수준.

### 소켓 맵 (Pico, W5500/W6300 0~7)
0=TG poll, 1=TG send, 2=LLM+프록시, 3=sendPhoto, 4=Vision, 5=DNS, 7=웹서버(port 80). **빈 소켓 = 6뿐.** dhcp_socket=0으로 설정돼 있으나 DHCP 미사용(NETINFO_STATIC 강제) — 켜는 순간 TG poll과 충돌.

### 빌드/보드 불일치
코드 주석·문자열 = W55RP20, 빌드 보드 = `W6300_EVB_PICO2`(RP2350+W6300), 빌드 폴더 = `build_W55RP20`. TLS 엔트로피 = RP2350 TRNG(`get_rand_32`) — **RP2040(W55RP20)엔 TRNG 없음, 포팅 시 교체 필수.**

### 보안 (공개 전 필수 처리)
- `wiz_claw_webserver.c:49` 기본 봇토큰 하드코딩 + 평문 HTTP 페이지에 API키/봇토큰 `value='%s'` 노출
- 구트리 `wiz_claw_agent/main.c` BOT_TOKEN/OPENAI_API_KEY 하드코딩
- CA 번들은 정상 동작 중(VERIFY_REQUIRED, GTS+GoDaddy 4장) — 단 파싱 실패 시 VERIFY_NONE 폴백 경로 존재, 번들 주석 오기

---

## 1. 핵심 결정: 재구성 불필요 — fork + 브랜치 재정렬로 간다

**새로 밀고 재구성하지 않는다.** 근거:
- 로컬 변경은 구조적으로 추가형(additive). 침습은 `claw_core` transport +143/-4 단 한 곳
- upstream 훅 파일 드리프트는 main.c +99줄 / transport.c +3줄 — rebase 충돌 관리 가능 수준
- 코드 자체는 esp-claw 스타일(AGENTS.md: opaque handle, esp_err_t) 이미 준수

단, **git 재정렬은 필수**다. 현재 상태(espressif 원본 직결 + 미커밋 뭉치 + CRLF 노이즈 751파일)로는 PR을 만들 수 없다.

전략 = 투트랙:
- **트랙 1 (WIZnet PoC 저장소)**: fork에 하이브리드 전체를 유지 — "working prototype" 증거
- **트랙 2 (upstream PR)**: 소형 PR부터 — ① transport hook 일반화 ② wired-bridge 컴포넌트

---

## 2. G-트랙: git 위생 (모든 작업의 선행 조건)

### G-1. 스냅샷 커밋 (양쪽 트리, 오늘)
- ESP: 실변경만 커밋 (CRLF-only 파일은 `git checkout --`으로 복원). 실변경 목록: `wiz_claw_spi_proto.h`, `spi_wiz.c`, `main/CMakeLists.txt`, `main/Kconfig.projbuild`, `main/main.c`, `claw_llm_http_transport.c/.h`, `sdkconfig.defaults`, dfrobot yaml + 미추적(seeed 보드, person_detect, 문서)
- Pico: `wiz_claw_spi_host/*` + `port/wiz-claw/*` + `port/mbedtls/inc/ssl_config.h` 커밋
- CRLF 재발 방지: `.gitattributes`에 `* text=auto eol=lf` (esp-claw 트리)

### G-2. fork 생성 및 리모트 재배선
```
GitHub: espressif/esp-claw → fork → wiznet(개인/회사)/esp-claw
git remote rename origin upstream
git remote add origin <fork URL>
```

### G-3. 브랜치 재구성 (스냅샷 후)
- `hybrid/w55rp20` ← 현 전체 상태 (PoC 트랙, upstream master 위 rebase)
- `feature/llm-transport-hook` ← transport.c/.h 변경만 체리픽 (PR#1 후보)
- `feature/wired-bridge` ← spi_wiz + Kconfig + main.c 연결부 (PR#2 후보, E-트랙 완료 후)

### G-4. 프로토콜 헤더 단일화
`wiz_claw_spi_proto.h`가 ESP/Pico 두 사본 — diverge 이미 한 번 발생(2048 vs 4096). 정본 하나 정하고 사본에 "GENERATED — do not edit" 헤더 + 비교 스크립트(CI든 pre-commit이든) 추가. 이때 `proto_ver` 1바이트 추가(헤더 7→8바이트, 양쪽 동시).

---

## 3. S-트랙: 안정성 검증 (사용자 요청 — 수정 전 베이스라인)

측정 없이 튜닝 금지. 순서:

### S-1. 계측 먼저
- Pico: LLM_REQ 수신→SPI 완료→DNS→TLS 완료→첫 바이트→응답 완료 타임스탬프 로그 (릴레이 1왕복당 1줄)
- ESP: proxy 요청→응답 타임스탬프. LLM_RESP 페이로드에 구간 시각 실어 양쪽 대조
- 이 로그로 "TLS 핸드셰이크 X초" 실측치 확보 — 이후 모든 개선의 before/after 기준

### S-2. 시나리오 매트릭스 (각 20회 이상)
| # | 시나리오 | 확인 항목 |
|---|---------|----------|
| 1 | WiFi on, 유선 연결 | 무선 직결 경로 회귀 없음 |
| 2 | WiFi off 부팅 → TG 대화 | 프록시 성공률, 왕복 시간 분포 |
| 3 | 대화 중 WiFi 절단/복구 | 모드 전환, g_use_spi_proxy 상태 |
| 4 | ESP 리셋 (Pico 유지) | g_esp32_alive 거동, 90s 타임아웃 낭비 확인 |
| 5 | 장문 응답 (>16KB) | 청크 조립, HTTP_PROXY_BODY_MAX(32KB) 경계 |
| 6 | 연속 요청 (프록시 직렬화) | s_spi_proxy_lock 30s, 힙 단편화(DEVLOG 9차 회귀) |
| 7 | PIR 트리거 중 TG poll | 5s 지연 실측, 60s 쿨다운 |

### S-3. 알려진 취약점 회귀 확인
DEVLOG 9~11차 픽스(동시 TLS 크래시, PSRAM 할당, TLS 핸드셰이크 무한루프) 각각 재현 시나리오 1회씩.

---

## 4. P-트랙: Pico측 성능/신뢰성 (S-트랙 베이스라인 이후)

우선순위순. 각 항목 완료 시 S-1 계측으로 before/after 기록.

- **P-1. TLS warm connection**: `s_tls` 단일 static → 소켓별 컨텍스트 분리, LLM 소켓(2) keep-alive 유지, `Connection: close` → `keep-alive`, 서버 절단 시에만 재연결
- **P-2. TLS 세션 재개**: `mbedtls_ssl_get_session/set_session` — 재핸드셰이크 비용 절감. (W55RP20 확정 시 P-1과 함께 필수)
- **P-3. DNS 캐시**: TTL 기반 static 2엔트리 (groq, telegram)
- **P-4. 송신 핫패스 printf 제거**: `wiz_spi_slave_send:240,260` (수신측은 이미 억제됨)
- **P-5. dead-ESP 감지**: ESP_STATUS 20s 주기 기반 — 3주기(60s) 무수신 시 `g_esp32_alive=false`, 수신 시 복귀. 히스테리시스 이걸로 충분, 신규 하트비트 커맨드 불필요
- **P-6. CRC-8 승격 + NACK 송신**: XOR→CRC-8, CRC 오류 시 NACK 즉시 반환(현재 조용히 드랍→상대 타임아웃 대기). proto_ver 올리는 G-4와 동시 진행
- **P-7. (선택) 고정 딜레이→ACK 기반 흐름제어**: ESP 20ms/15ms 제거. 청크 4096 확대는 양쪽 헤더+Core1 static 버퍼+ESP DMA 동시 변경이므로 P-6 이후
- **P-8. (선택) SSE 스트리밍 포워딩**

## 5. E-트랙: ESP측 컴포넌트화 (upstream 준비)

- **E-1. transport hook 일반화**: `claw_llm_spi_proxy_fn_t` → 트랜스포트 중립 네이밍(`claw_llm_http_transport_override` 류), SPI 언급 제거. diff를 claw_core 기준 수십 줄로 압축 → **PR#1**
- **E-2. wired-bridge 컴포넌트화**: main.c 산재 로직(+168줄: on_spi_rx 디스패치, spi_http_proxy_fn, ping/status 태스크) → `components/spi_wiz` 확장 또는 신규 `wiz_wired_bridge` 컴포넌트로 흡수. 자체 Kconfig(핀/클럭/백엔드 선택) 보유. main.c에는 create/start/콜백 등록만 남김 → **PR#2 또는 WIZnet-maintained**
- **E-3. ESP_STATUS 실상태 반영**: 하드코딩 `{"wifi":true,"agent":true}` → 실제 wifi/agent/proxy 상태 + 확장 필드
- **E-4. rebase onto upstream master**: 83커밋 따라잡기. 충돌 예상 지점: main.c(양쪽 수정), transport.c(+3줄 경미)

## 6. 보안 체크리스트 (PoC 공개 전, 순서 무관 필수)

1. `wiz_claw_webserver.c:49` 하드코딩 봇토큰 제거 → **해당 토큰 BotFather에서 로테이션** (git 이력에 이미 존재하므로 제거만으로 부족)
2. 구트리 wiz_claw_agent의 API키/토큰 동일 처리
3. 대시보드: 저장은 유지, 조회 시 마스킹(`****`, 변경 시에만 입력)
4. VERIFY_NONE 폴백 → 부팅 경고 + 연결 거부(또는 Kconfig opt-in)
5. CA 번들 주석 수정 (실제: GTS×2 + GoDaddy×2)

## 7. 업스트림 로드맵 (기존 계획 유지, 순서 명시)

1. G-트랙 완료 → WIZnet fork에 PoC 공개 (블록도·영상·S-트랙 측정치 포함)
2. esp-claw에 Feature Proposal Issue — "working prototype" + 측정 데이터 명시
3. 반응 오면 인터페이스 위치 합의 → PR#1(E-1) 제출
4. PR#2(E-2)는 합의된 위치로. 병합 불발 시 fork를 WIZnet-maintained integration으로 유지
5. 참고: upstream에 `rust-migration` 브랜치 존재 — 제안 시점에 maintainer에게 C 트리 로드맵 확인할 것

## 8. 결정 대기 (착수 전 답 필요)

1. **최종 타깃 보드**: W55RP20(RP2040) 확정이면 P-1/P-2 필수 + TRNG 대체 태스크 추가. RP2350/W6300 유지면 P-1~P-3만으로 충분 가능 — S-트랙 실측 후 판단 권장
2. **방향 A(소켓 브릿지, TLS를 ESP로 환원) 채택 여부**: 구 TASK_BRIEF의 "ESP는 네트워크 스택을 몰라야 한다"와 모순. 현 Mode B 유지 + P-트랙 튜닝이 기본안, 방향 A는 S-트랙 실측이 목표 미달일 때 재론
3. **fork 계정**: 개인 vs 회사 GitHub

## 9. 금지 사항

- D:\PICO2 구트리 수정 금지 (폐기)
- G-1 스냅샷 전 리팩터 금지
- 보안 체크리스트 완료 전 저장소 공개 금지
- 프로토콜 헤더 한쪽만 수정 금지 (G-4 단일화 후 proto_ver 동반)
