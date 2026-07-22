# DEVLOG

날짜별 개발 로그. 최신 항목이 위에 온다.

> ★ 방향성/북극성은 `TASK_BRIEF_WIRED_V2.md`의 "★ 북극성 & U-트랙" 단일 진실원 참조.
> 최종 목적 = esp-claw에 WIZnet 유선 솔루션 **알리고 안착**(사설 데모 완성 아님).
> 정체성 = **fault-domain 격리**(연산 ESP ↔ 네트워크/관리 W55RP20). 매 세션 U-트랙 전진
> 여부 먼저 점검 — 내부 견고화만 반복하며 산으로 가지 않기 위함. 코어 버그는 쫓지 말고
> Guardian이 완화(감지·복구)하는 대상으로 남길 것.

---

## 2026-07-22 (U-트랙 28차) — Lightweight 모드 힙 크래시 소멸 확인 (대성과) + 잔존 극단 케이스

27차 회피책(Lightweight 메모리 모드) 빌드·플래시하고 안테나 탈거 연타 재현. **힙손상
크래시 완전 소멸 확인 — 오늘 최대 성과.**

**Lightweight 모드 검증 (성공)**
- ESP 로그 전체(up 85~745s, LLM_REQ 1~10) **크래시 0회.** 이전엔 몇 요청 만에
  `heap_caps_free assert`/`Guru Meditation`으로 패닉 리부트였으나 이제 10개+ 견딤.
  `Rebooting` 없음.
- 모드 반영 확인: `Long-term Memory context_len=73`(FULL 511 — markdown만 읽음,
  cJSON 경로 없음), `cap Tools 11566`(FULL 12970 — memory 툴 제외), caps 9그룹
  (FULL 10), auto-extract 로그 소멸. → crash 경로 자체가 빌드에서 빠짐.
- 프록시 픽스도 재확인: `failing fast`, req 6~10 프록시 timeout이 25s 내 종료,
  ESP 생존, STATUS 계속(seq 29→33).

**잔존 이슈 (우선순위 낮음, 데모 영향 적음)**
- Pico 로그 뒷부분: 안테나 완전 제거 + ~20개 연타의 극단 스트레스에서, 어느 순간부터
  Pico가 프록시 응답을 못 만들고(openai 아웃바운드 실패 추정) → relay streak 누적 →
  HUNG/DEAD → reset. 앞서(25/26차) 논의한 Pico 소켓/네트워크 계열 결함으로 추정.
- 단 "인터넷 완전 부재 + 장시간 연타" 극단 케이스. 정상 WiFi/짧은 끊김에선 완벽
  (동일 로그 앞 req 1~5 전부 성공). 데모 시나리오에서 회피 가능.

**전시회 상태 요약**: 치명적·잦은 힙 크래시 = 해결(Lightweight). 프록시 블로킹/가짜
DEAD = 해결(검증). 잔존 = 인터넷 완전부재 극단 케이스만. **데모 안정성 확보 수준 도달.**

**다음**: (1) 지금 안정 버전을 양측 커밋 = 전시회 세이프포인트 확보(U-1 + 프록시픽스 +
`__hang__` 게이팅 + Lightweight sdkconfig). (2) 이후 잔존 극단 케이스(Pico 프록시
아웃바운드) 계속. (3) 데모 시나리오 리허설.

---

## 2026-07-22 (U-트랙 27차) — 프록시 픽스 검증 성공 + 진짜 크래시 = 코어 힙손상(claw_memory) → Lightweight 회피

26차 프록시 픽스를 빌드·플래시하고 안테나 탈거 연타로 재현. **프록시 픽스는 성공
확인**, 그리고 그동안 가려져 있던 **진짜 크래시 원인이 ESP 코어 힙손상**임이 백트레이스로
확정됨.

**26차 프록시 픽스 검증 (성공)**
- 로그에 신규 문구 그대로: `SPI proxy failed (ESP_ERR_TIMEOUT) — failing fast (WiFi
  down, no direct fallback)`. req=9/10/11이 프록시 timeout 시 직결·인라인 재시도 없이
  즉시 실패 반환. 각 요청 ~25s 내 종료.
- 그동안 STATUS seq 계속 증가(13→27), DEAD 안 걸림. **"90초 블로킹 → 가짜 DEAD →
  리셋" 사슬 제거 확인.** 프록시 레이어 결함 해결.

**진짜 크래시 = 코어 힙손상 (백트레이스 확정, 우리 스코프 밖)**
```
assert failed: heap_caps_free (heap != NULL && "free() target pointer is outside heap areas")
cJSON_Delete → claw_memory_long_term_collect (claw_memory.c:769)
      → claw_core_build_iteration_context → claw_core_agent_loop_task
reset_reason=4 (panic)
```
- 25차에 기록한 그 힙손상 버그. `claw_memory_long_term_collect`가 매 요청 컨텍스트
  빌드 때 `index_root`(cJSON)를 파싱→`cJSON_Delete`하는데, 그 지점에서 assert.
  함수 자체는 정상(summaries는 detach 안 한 자식, index_root만 delete — double-free
  없음) → **crash 지점 ≠ 버그 지점**. 힙이 이전에 딴 데서 손상됨(auto-extract가 쓴
  index 파일 손상 의심). esp-claw 코어 결함, HEAP_POISONING 추적 필요 = 전시회
  데드라인엔 부적합.
- 크래시 후 동작은 설계대로: Guardian DEAD 감지 → 리셋 → 재부팅 → **20차 mid-relay
  큐 실전 발동**("기기가 재시작됐어요. 복구되면 자동으로…" → 자동 재질의) → 정상 복귀.
  불완전 코어 위 생존 = 프로젝트 가치 증명(전시회 데모 포인트).

**전시회 안정화: Lightweight 메모리 모드로 크래시 경로 회피 (코드 수정 0)**
- 발견: 이미 Kconfig 스위치 존재. `app_claw.c:376` — `CONFIG_APP_CLAW_MEMORY_MODE_FULL`
  이면 crash 나는 `claw_memory_long_term_provider`, 아니면
  `claw_memory_long_term_lightweight_provider` 등록.
- `claw_memory_lightweight.c`의 lightweight collect는 **cJSON 전혀 안 씀** —
  markdown 파일만 `read_file_dup`으로 통째 읽음. crash 경로(cJSON_Delete 힙손상)가
  이 모드엔 아예 없음. + "without structured auto extraction" → index 쓰기/손상
  경로도 차단.
- **조치(사용자)**: `idf.py menuconfig` → App Claw Config → memory mode →
  **Lightweight** → storage 파티션 erase(손상 index 제거, NVS 보존) → 빌드/플래시 →
  안테나 뽑고 연타 재현으로 크래시 소멸 확인.

**현재 상태 요약(전시회 관점)**: 프록시 블로킹/가짜 DEAD = 해결(검증됨). 코어 힙손상
크래시 = 근본 미해결(코어 스코프)이나 Lightweight 모드로 발현 경로 회피 + Guardian
자동복구가 안전망. 이 조합이면 데모 안정성 확보 가능.

**다음**: Lightweight 재현 테스트 결과 확인 → 크래시 사라지면 U-1/프록시픽스/메모리모드
전부 양측 커밋 → 전시회 데모 시나리오(정상 대화 + 의도적 WiFi 다운 → 유선 프록시 →
크래시 시 자동복구) 리허설.

---

## 2026-07-21 (U-트랙 26차) — 결함 원인 규명: ESP 프록시 이중시도 → 90초 블로킹 → 반죽음

25차의 "Pico RX 사망" 가설을 파려고 로그를 더 잡았더니 **가설이 틀렸음이 확인되고
진짜 원인이 잡힘.** SPI race도, Pico 결함도 아니라 **ESP의 프록시 재시도 로직**.

**가설 폐기 근거**: 재현 로그에서 SPI/STATUS가 초반엔 멀쩡(seq 정상 증가) → race로
RX가 죽었다면 STATUS부터 끊겼어야 하는데 안 그럼. 대신 특정 요청(ESP req=9)부터
무너지기 시작.

**확정된 고장 사슬 (Pico 로그로 관측):**
1. 프록시 200 응답까지 정상 → 근데 그 뒤 ESP가 LLM_RESP(0x43)를 Pico에 안 보냄
   (`[relay] timeout after proxy activity`).
2. 다음 요청부터는 HTTP_REQ(0x06)조차 시작 안 함 (`timeout — falling back` streak 1→4).
3. 결국 STATUS(0x44)까지 멈춤 → 65s → `esp=DEAD` → Guardian 리셋. **ESP의 SPI 송신
   전체가 죽음(LLM_RESP+HTTP_REQ+STATUS 전부). ESP 반죽음.**
   - 이번 DEAD 판정·리셋은 **오판이 아니라 정당** — ESP가 실제로 송신 불능이었음.
     Guardian이 제 역할 함.

**근본 원인 (코드 확정):**
- `components/.../llm/claw_llm_http_transport.c`: WiFi 다운 상태(`g_use_spi_proxy`)에서
  LLM 요청 1건 처리가 최악의 경우:
  1. 1차 SPI 프록시 시도 → Pico 응답 세마포어 **45s** 대기(`spi_http_proxy_fn`
     `main.c:177`, `xSemaphoreTake(..., 45000)`) → timeout.
  2. `g_use_spi_proxy=false` → **직결 HTTP 시도**(WiFi 없으니 무의미, 즉시 실패,
     transport.c:379~463).
  3. **인라인 프록시 재시도**(transport.c:469~484) → `g_use_spi_proxy=true` → 같은
     요청 **또 45s** 대기 → 또 timeout.
  → **한 요청에 최대 ~90s 블로킹** + 그동안 SPI 송신 경로 반복 점유 → STATUS/LLM_RESP가
    밀림 → Pico STATUS 65s 끊김 → DEAD.
- (STATUS가 정확히 어느 뮤텍스에서 막히는지는 `spi_wiz` 컴포넌트 내부까지 봐야 100%
  확정 — 단 픽스 방향은 무관하게 확실: 90s 블로킹 자체를 제거.)

**픽스 설계 (다음 세션 구현):**
1. **핵심**: 이미 프록시 모드인데 1차 프록시가 `ESP_ERR_TIMEOUT`이면 직결 시도 +
   인라인 재시도를 **둘 다 건너뛰고 즉시 실패 반환**. WiFi 없는 게 확실한데 같은
   요청을 45s 더 반복해봤자 동일 timeout. `transport.c`의 inline-retry 진입 조건에
   "직전 프록시가 timeout이 아니었을 때만" 가드 추가.
2. `spi_http_proxy_fn` 응답 대기 45s → 25s (Pico 왕복 실측 ~15s + 여유). 단일 실패의
   최대 블로킹을 절반으로.
3. (검토) STATUS 송신을 프록시 블로킹과 독립 보장 — 별도 우선순위 or 프록시 대기가
   SPI 송신 뮤텍스를 잡지 않도록.

**오늘 세션 종료 판단**: U-1 완결 + CRC 픽스 + 훅 게이팅 + 본 결함 원인규명까지 큰
진전. 본 픽스는 다중 파일·신중 수정 + 빌드/재테스트 필요 → 다음 세션에 코드 작성 →
사용자 빌드. 오늘은 양쪽 전원 재인가로 정상 복귀만.

**다음 세션 최우선**: 위 픽스 1·2 구현(ESP `claw_llm_http_transport.c` +
`main.c` spi_http_proxy_fn) → 안테나 뽑고 연타 재현으로 "90s 블로킹/DEAD" 소멸 확인.
그 후 24차까지의 U-1 성과 커밋 → U-2.

---

## 2026-07-21 (U-트랙 25차) — 훅 게이팅 빌드검증 + 카오스 테스트로 결함 3건 발견

24차 마무리로 `__hang__` 훅 Kconfig 게이팅(기본 n) 빌드·플래시 검증. 이후 안테나
탈착 반복하며 여러 실패 시나리오를 우연히 밟아 신규 결함 3건 확보. 우리 레이어 기능
(mid-relay 리부트 큐, 유선 인라인 폴백)이 실전에서 맞물려 자가복구하는 장면도 처음
포착.

**훅 게이팅 검증 (통과)**
- `idf.py build` 정상, `__hang__` 코드 통째 컴파일 제외 확인(Kconfig 기본 n).
- 플래시 후 `[guardian] reset-request line ready on GPIO4` 정상 — 훅만 빠지고 나머지
  guardian 인프라 온전. TG로 "hang" 보내니 LLM이 정상 답변(훅 비활성 확인).

**신규 결함 ①(1순위, 우리 스코프): Pico 완전 락업 + Guardian 사각지대**
- 안테나 뽑은 상태 연속 요청 중 Pico가 프록시 아웃바운드 TLS handshake
  (`api.openai.com:443`)에서 멈춤 → **Pico `[health]` 1초 타이머 출력까지 정지 =
  코어 완전 하드행**(네트워크 느림이 아니라 락업). ESP는 정상(heap/seq 계속 증가).
- 유발 정황: ESP가 첫 프록시 타임아웃 후 `retrying via SPI proxy inline`으로 동일
  요청 2차 발사 → Pico가 1차 처리 중 TLS 슬롯(sn=0/1/2) 겹침. 6/22~23(3·10·11차)
  "동시 TLS handshake hang" 계열 재발로 추정.
- **구조 문제 2개 노출**:
  (a) Pico 릴레이 90s 데드라인은 while 루프 반복 사이에만 체크 → 루프 안
      `wiz_claw_http_post_cb()` 블로킹 콜이 TLS에서 멈추면 데드라인 영영 미도달
      (사실상 무제한 대기).
  (b) **리셋선은 Pico→ESP 단방향.** Pico 자체가 죽으면 되살릴 주체가 없음. Guardian은
      ESP 사망만 감지·복구하고 Pico 사망은 감지 자체가 없음(heartbeat 비대칭).
- 복구: Pico 전원 재인가 필요(SW 복구 불가).

**신규 결함 ②(1순위, 우리 스코프): PING/PONG 단발성 → Pico 단독 리부트 시 링크 좀비화**
- ESP는 **자기 부팅 시 1회만** PING 재시도 루프. Pico가 PONG 주면 종료, 이후 재확인
  없음. → Pico만 리부트하면 ESP는 "핸드셰이크 완료" 상태 유지한 채 죽은 링크에 계속
  송신. 양쪽 다 리셋해야만 재동기.
- 픽스 방향(다음): ESP 주기적 재핑 **또는** Pico가 (재)부팅 시 능동 "reboot 알림"
  패킷 송신 + ESP 상시 리스닝. 결함 ①(b)의 반대 방향 heartbeat와 함께 "양방향
  liveness"로 묶어 설계.

**신규 결함 ③(코어 스코프, 완화 대상): WiFi 다운 순간 ESP 크래시**
- 안테나 있이 부팅→응답 성공→**HTTP 요청 진행 중 안테나 탈거** 순간 ESP
  `Guru Meditation Error: Core 1 panic (IllegalInstruction)`, `memcpy in ROM`
  백트레이스. 진행 중 TLS/HTTP 처리 메모리 손상 — esp-tls/esp_http_client 계열,
  **esp-claw 코어 스코프(우리가 안 고침, Guardian이 완화)**.
- **자가복구 확인(수확)**: ESP 자체 panic 핸들러로 재부팅(reset_reason=4, Guardian
  무관) → 재부팅 중 **20차 mid-relay 리부트 큐가 실전 첫 발동**: TG에
  `기기가 재시작됐어요. 복구되면 자동으로 다시 답해드릴게요.` → 재부팅 후 WiFi 자동
  재접속 → 직결 실패 시 유선 인라인 프록시 자동 전환까지 연쇄 정상 동작. 예정 없던
  카오스 테스트에서 기존 회복 기능들이 물려 돌아가는 걸 실증.

**부수 관찰**: `apply cached context failed ... Session History err=ESP_FAIL`
(첫 메시지/세션 인덱스 파일 부재 `errno=2` 계열)로 요청 1건 실패, ok=0 우아한 처리.
치명적 아님, 코어 세션 저장 계열.

**다음**: 결함 ①·② = 양방향 heartbeat/liveness로 묶어 설계·구현(우리 스코프,
U-트랙 핵심 — "Pico도 죽을 수 있다"는 fault-domain 대칭성 보강). ③은 코어라 기록만.
그 전에 24차까지의 U-1 성과 커밋 여부 결정.

---

## 2026-07-21 (U-트랙 24차) — U-1 완결: HUNG→RESET→RECOVERY 풀사이클 실기 검증 + OpenAI 전환

23차 CRC 픽스 이후 같은 날 이어서: LLM 프로바이더를 groq→OpenAI(`gpt-4o-mini`, 회사
발급 `sk-proj-` 키)로 전환하고, 23차에서 미완이었던 `__hang__` 실 임계값 HUNG 시뮬을
드디어 정확히 재현해 **U-1(Guardian 리셋선)을 하드웨어+로직 전체 완결**.

**LLM 프로바이더 전환 (groq → OpenAI)**
- groq 계정의 `meta-llama/llama-4-scout-17b-16e-instruct`가 라인업에서 완전히
  빠짐(단종) — 키 만료가 아니라 모델 자체가 없어진 것. groq 대시보드 모델 목록으로 확인.
  키 재발급 불필요.
- 회사 발급 OpenAI 프로젝트 키(`sk-proj-...`)로 전환: base_url
  `https://api.groq.com/openai/v1` → `https://api.openai.com/v1`, model → `gpt-4o-mini`.
  esp-claw 프로비저닝 UI의 "OpenAI" 프리셋 사용.
- 전환 후 안테나 뽑은 상태로 9연속 요청 테스트 — 8/9 정상 응답, `[health]` 끊김 없음.
  덤으로 WiFi 끊기는 순간을 라이브로 포착: req=2에서 직결 HTTP가
  `Connection reset by peer`로 실패 → `HTTP failed, retrying via SPI proxy inline` →
  유선 경유 자동 성공. WiFi가 아직 "죽었다"고 안 알려진 찰나에 코드가 알아서 유선으로
  전환하는 장면 — U-4 데모 소재 확보(20차의 groq 버전과 같은 패턴, 프로바이더 무관하게
  재현됨 → 유선 페일오버가 프로바이더 특정 우연이 아니라 설계대로 동작한다는 방증).

**CRC 재동기 픽스 회귀 검증**
- 9연속 + 이후 다회 요청(최대 29KB body)에서 CRC 에러 자체는 재현 안 됐지만(확률
  낮은 이벤트), 23차 변경 이후 회귀 없이 전부 정상 — 안정성 유지 확인.

**`__hang__` 트리거 실전 삽질 → 해결**
- 텔레그램 클라이언트가 `__hang__`을 입력 즉시 이탤릭 마크다운으로 **변환하며 밑줄을
  삭제**해 버림 → 봇은 그냥 `hang`을 받아 LLM이 진짜로 답변(2회 반복 실수).
- **해결**: 백틱으로 감싸서 입력(`` `__hang__` ``) — 코드스팬 안에서는 중첩 마크다운
  파싱이 안 돼 밑줄이 리터럴로 보존됨. 이후 정상 트리거.

**U-1 풀사이클 검증 (실 임계값)**
```
streak=1,2 → (판정 없음)
streak=3   → [guardian] ESP32 agent HUNG (STATUS alive)   ← RELAY_HUNG_STREAK=3
streak=4   → HUNG 반복
streak=5   → HUNG + [guardian] reset triggered (reason=HUNG)
           → reset pulse #1 sent to ESP32 (GPIO5 high 100ms)  ← GUARD_RESET_STREAK=5
ESP:  [guardian] reset request from W55RP20 confirmed → esp_restart()
      reset_reason=3 (RTC_SW_CPU_RST) → 재부팅 → WiFi 재접속(1s, 저장 크리덴셜)
      → 전 서비스 재초기화 → LLM 백엔드 복원 → SPI PING/PONG 재개
```
20차 이후 처음으로 **오탐 없이 설계된 정확한 임계값에서만** HUNG 판정과 리셋이 발동하는
것을 확인. 하드페일 래치(3회 누적) 미도달, 정상 1회 리셋 후 완전 복구.

**U-1(Guardian 리셋선) = 완결.** 배선, FW 양측, HUNG/DEAD 양 경로 실기 검증, 정확한
임계값 재현까지 전부 끝남.

**`__hang__` 훅 처리 확정**: 삭제도 방치도 아니고 Kconfig로 게이팅. `main/main.c`의
`if (strstr(text, "__hang__"))` 블록을 `#ifdef CONFIG_CLAW_GUARDIAN_HANG_TEST_HOOK`로
감싸고, `main/Kconfig.projbuild`에 `App Config → Wired Guardian Test Hooks → Enable
Guardian HUNG regression test hook` 옵션 신설(기본 n). 이유: 무조건 트리거되는 문자열을
프로덕션 빌드에 남기면 누구든 텔레그램으로 `__hang__` 보내 에이전트를 90s 먹통 + 결국
ESP 강제리셋시킬 수 있음(공개 예정 프로젝트라 더 중요) — 그렇다고 삭제하면 U-1 로직을
다시 건드릴 때마다 회귀 재현 수단이 없어짐. 기본 빌드엔 안 들어가고, 필요할 때
`idf.py menuconfig`로 켜서 재현 가능.

**다음**: 양측(ESP `hybrid/w55rp20`, Pico `main`) 미커밋 변경사항 커밋 → U-2 벤치마크
표(vs dumb 워치독) 착수.

---

## 2026-07-21 (U-트랙 23차) — SPI RX 영구 사망 결함 픽스: 타임아웃 재동기

22차에서 발견한 결함(CRC 에러 1회 → RX 영구 사망) 근본 원인 규명 + 픽스. Pico
`wiz_claw_spi_host/wiz_spi_slave.c`만 수정, ESP 무변경.

**근본 원인**: `_handle_rx()`가 헤더의 `len` 필드를 **CRC 검증 전에** 신뢰해서
`_rx_bytes(payload, plen)`으로 그만큼 무기한 블로킹 리드함. `len` 필드 자체가 전송
중 비트 손상되면(`SPI_CLAW_MAX_CHUNK`=2048 이하 범위 내라 크기 가드도 못 잡음) Pico는
실제 ESP가 보낸 바이트 수보다 많이 읽으려 시도. ESP는 이미 그 프레임 전송을 끝내고
클럭을 멈췄으므로 Pico는 무기한 대기 — 그러다 **ESP가 다음에 보내는 진짜 프레임의
선두 바이트를 이전 프레임 payload의 나머지로 먹어버림**. 그 뒤로 모든 프레임이
영구적으로 오프셋이 밀려 CRC가 항상 불일치하고, 우연히 0xCA 0xFE로 재정렬될 확률이
사실상 0에 가까워 복구 불가. "CRC 에러 1회 → RX 영구 사망, TX(LLM_REQ 송신)는 정상"
관측과 정확히 일치(TX는 `wiz_spi_slave_send()`로 별도 경로, poll() 상태와 무관).

**픽스**: `_rx_bytes()`(무기한 블로킹)를 `_rx_bytes_timeout()`(100ms 데드라인,
`to_ms_since_boot(get_absolute_time())` 기준)로 교체. 실패 시 CRC 체크를 건너뛰고
즉시 리턴해 poll()의 매직바이트 스캔이 다음 진짜 프레임에서 재동기할 기회를 보존.
3곳 적용:
1. `_handle_rx()` 헤더 잔여 5바이트 읽기
2. `_handle_rx()` payload 읽기 (가장 중요 — 사고의 직접 원인)
3. `wiz_spi_slave_poll()`의 2번째 매직바이트 대기(우연한 0xCA 매칭 뒤 더 이상
   바이트가 안 오는 경우도 같은 취약점이라 함께 적용)

기존 `_rx_bytes()`는 이제 미사용이라 삭제(20차 "unused-static 경고 무시 금지" 교훈
반영 — 경고를 남겨두지 않음).

**한계**: 타임아웃은 "다음 프레임을 훔쳐 먹는 것"을 막을 뿐, 애초에 CRC 검증 전에
`len`을 신뢰하는 프로토콜 설계 자체는 여전함. 완전한 해법은 헤더 전용 체크섬을
따로 둬서 payload 길이를 읽기 전에 헤더 무결성부터 검증하는 것(ESP측 프로토콜
변경 필요) — 지금은 스코프상 보류, 필요 시 별도 트랙.

**다음**: 빌드→플래시→대용량 연속 프록시로 CRC 유발 재현 테스트(재동기 확인)→
통과 시 `__hang__`(정확한 문자열) 5연발 HUNG 시뮬 재개.

---

## 2026-07-16 (U-트랙 22차) — U-1 리셋선 E2E 실기 검증 성공 (HUNG·DEAD 양 경로) + 신규 결함: CRC 후 SPI RX 사망

21차 FW를 빌드·배선하고 하루 종일 실기 테스트. **리셋선이 두 경로(HUNG/DEAD) 모두에서
실제로 ESP를 되살리는 것 확인** — U-1 하드웨어 검증 완료. 부산물로 오탐 사고 1건(수정
완료)과 신규 결함 1건(SPI RX 사망) 확보.

### 사고 1: DEBUG 값 미복원 → 멀쩡한 ESP 리셋 루프 (수정 완료)

U-1 빠른 테스트용으로 낮춰둔 값 3개("REVERT before commit" 주석까지 달아놓고)를
복원하지 않은 채 실 메시지 테스트 진행:
- `LLM_RELAY_TIMEOUT_MS` 90000→8000: 프록시 왕복 실측 ~10.5s > 8s → **모든 릴레이가
  구조적으로 타임아웃**. LLM_RESP는 매번 뒤늦게 도착해 버려짐.
- `RELAY_HUNG_STREAK` 3→2, `GUARD_RESET_STREAK` 5→2: 메시지 2개 만에 HUNG 판정+리셋.
- 결과: 유저는 답을 영원히 못 받고, guardian은 일하는 중인 ESP를 리셋. 단, 이 오탐
  리셋이 **펄스→ISR→`esp_restart()`→`reset_reason=3`(RTC_SW_CPU_RST) 체인의 첫 실기
  검증**이 됨(전화위복).

**픽스 (Pico `main.c`)**: 3값 복원 + **proxied-liveness 가드** 신설 — relay 대기 중
ESP가 프록시 HTTP를 실제로 서빙했으면(= 에이전트가 이 요청을 처리 중이라는 직접 증거)
타임아웃이 나도 HUNG streak에 카운트하지 않음(`timeout after proxy activity — agent
busy, not hung`). "느림"과 "행업"의 구조적 분리.

### 검증 A: 정상 릴레이 (픽스 후)

TG 3연속 → relay_ok=3, streak=0, 회당 ~10s. 픽스 유효.

보너스 장면: ESP가 wifi=1인데 groq 직결 실패(`ESP_ERR_HTTP_CONNECT`) →
`llm_http`가 **inline SPI 프록시로 재시도 → 성공**. "WiFi는 붙었지만 인터넷 실질
불능" 케이스를 유선이 구제 — U-4 데모 소재 1급.

### 검증 B: "hang" 테스트 (의도와 다르게 흘렀지만 수확 큼)

`__hang__` 훅을 쓰려던 게 키워드를 `hang`으로 보내서 **HUNG 시뮬은 미실행**
(1~2번째는 LLM이 진짜로 답함). 대신:

1. **proxied-liveness 가드 실전 발동 확인**: 3번째 메시지에서 프록시 200 서빙 후
   LLM_RESP가 유실(아래 결함)됐는데, 가드가 정확히 "busy, not hung"으로 streak 0 유지.
2. **신규 결함 — CRC 에러 1회 → Pico SPI RX 영구 사망**:
   `[spi_slave] CRC error: got 0x00, expected 0x3E` 이후 Pico가 **모든 수신 프레임
   상실**(STATUS 0x44, HTTP_REQ 0x06, LLM_RESP 0x43 전부). TX 방향(LLM_REQ 송신)은
   정상 — ESP는 req=4,5를 받아 처리 시도(프록시 요청이 Pico에 안 닿아 45s×2 타임아웃
   후 실패). 3~4차(6/22-23)의 "CRC 후 0xF0 디싱크"와 같은 계열 — 당시 완화(청크 축소,
   딜레이, printf 억제)로 빈도만 줄였지 **CRC 후 재동기 로직 부재**가 근본 원인.
   슬레이브 상태머신이 헤더 탐색으로 복귀 못 하는 것으로 추정. **결함 큐 1순위 등록.**
3. **DEAD 경로 E2E 검증 성공**: RX 사망으로 STATUS 65s 침묵 → `esp=DEAD` → 리셋
   펄스 #1 → ESP `reset request from W55RP20 confirmed → esp_restart()` →
   `rst:0xc (RTC_SW_CPU_RST)`. **SPI가 완전히 죽은 상태에서 GPIO 리셋선만으로 복구
   개시** — out-of-band 리셋의 존재 이유를 실증. 차별점 서사(대역외 소생 라인)의
   핵심 증거 확보. (이번 DEAD는 실제로는 Pico 쪽 링크 결함이었지만, "링크가 죽으면
   일단 상대를 리셋해 재동기 기회를 만든다"는 동작 자체는 설계 의도대로.)

### 오늘 확정된 것

- U-1 리셋선: 배선(GPIO5→GPIO4, GND 공유) + FW 양측 + HUNG(오탐이지만 체인 검증)·
  DEAD 양 경로 실기 동작 = **하드웨어 파트 완료**.
- 남은 U-1 마무리: `__hang__` 정확 키워드로 실 임계값(streak 3 판정/5 리셋) HUNG 시뮬
  + 하드페일 래치(3회) 검증 — RX 사망 결함과 무관하게 가능(LLM_REQ 방향은 살아있으므로,
  단 깨끗한 부팅 상태에서).

### 결함 큐 갱신 (우선순위)

1. **(신규·1순위) CRC 후 SPI RX 영구 사망** — Pico `wiz_spi_slave.c` CRC 에러 처리에
   재동기 없음. 방향: CRC 실패 시 프레임 상태 리셋 + 슬라이딩 윈도우 헤더 재탐색 +
   (선택) ESP에 NAK/재전송. 단일 비트 에러가 세션 전체를 죽이는 현 상태는 유선
   신뢰성 서사에 정면 배치 — U-트랙 진입 전 필수.
2. 로컬 폴백 키 미설정(기존 1번) — 이번에도 매 타임아웃마다 `local fallback
   unavailable` 노출.
3. /fatfs 누적(used 159744→188416 계속 증가), 힙손상·툴400 등 코어 이슈는 기존 입장
   유지(Guardian 완화 대상).

### 다음 세션

1. CRC 재동기 픽스(Pico `wiz_spi_slave.c`) → CRC 유발 테스트(대용량 연속 프록시).
2. `__hang__` 5연발 실 임계값 HUNG 시뮬 + 하드페일 래치.
3. 통과 시 U-1 완료 커밋(양측) → U-2 벤치마크 표 착수. 오늘 로그 2벌은 U-2/U-4 재료로 보관.

---

## 2026-07-16 (U-트랙 21차) — U-1 Guardian 리셋선 (FW 구현, HW+테스트 대기)

**U-트랙 첫 전진.** 차별점 MVP의 마지막 다리 = Guardian이 hung ESP를 물리적으로
되살리는 리셋선. 20차까지 감지만 하고(HUNG/DEAD) 되살리진 못했음 → 이번에 구현.

**설계 결정: CHIP_PU(HW EN) 대신 broken-out GPIO 소프트 리셋.**
사용자가 XIAO ESP32-S3 CHIP_PU 패드 납땜이 어렵다고 함. 트레이드오프 검토 후 GPIO
소프트 리셋 채택:
- HUNG 정의 = "STATUS는 계속 오는데(RTOS 살아있음) 에이전트만 응답 안 함." RTOS
  살아있으면 ESP GPIO ISR 발동 가능 → `esp_restart()` 호출됨. **차별점 케이스
  (워치독 IC가 못 보는 논리적 행업)를 완벽 커버.**
- 못 잡는 것: RTOS까지 완전 사망 + ISR 죽은 희귀 케이스(보통 패닉→자동재부팅). 이건
  물리 EN 라인 필요 → 향후 HW 옵션으로 문서화.
- **차별점의 지능은 리셋 핀 종류가 아니라 "논리적 행업 감지"에 있음** → SW 리셋으로 서사 유지.

**배선**: Pico GPIO5(출력, idle LOW) → ESP32-S3 GPIO4/D3(입력, 풀다운, posedge ISR),
GND 공유. 리셋 명령 = GPIO5 HIGH 100ms → ESP ISR → 50ms 재확인 → `esp_restart()`.
(HIGH 지속 확인으로 노이즈 오리셋 방지.) 핀 선정: GPIO4는 스트래핑 아님, SPI(1/2/7/8/9)·
PIR(44)와 안 겹침.

**ESP측** (`edge_agent/main.c`): `ESP_RESET_REQ_GPIO 4` + `reset_req_isr_handler`
+ `reset_req_task`(notify 대기→50ms 샘플→`esp_restart()`). SPI 브릿지 블록 안에 배치
(기존 PIR ISR 서비스 재사용).

**Pico측** (`wiz_claw_spi_host/main.c`):
- `esp_reset_pulse()`: GPIO5 HIGH 100ms.
- `guardian_try_reset(reason)`: 쿨다운 30s(부팅+접속 예산) + 최대 3회 하드페일 래치
  (`g_esp_hard_failed`) — 부트루프 방지. 초과 시 리셋 중단 + 로그.
- `guardian_note_recovery()`: relay 성공(에이전트 응답=생존 증명) 시 카운터 클리어.
  STATUS만으로는 클리어 안 함(HUNG일 때도 STATUS는 옴 → 오탐 방지).
- 트리거 2경로: HUNG(relay_to_streak ≥ 5, 감지 임계 3보다 높게 → 회복 기회) +
  DEAD(STATUS 65s 끊김, `check_esp32_alive_timeout`).
- 회복 확인은 기존 `g_esp_boot_epoch`(ESP 부팅 PING) + 메시지큐 자동 재질의와 맞물림
  → "죽었다 살아나며 답 전달" 데모 완성.

**상태**: FW 양측 완료·미빌드. **다음 = 사용자 빌드/플래시 → HW 배선(GPIO5↔GPIO4,GND)
→ HUNG 시뮬 테스트**(에이전트만 멈추고 STATUS 살림 → Pico 리셋 펄스 → ESP 재부팅 →
메시지큐가 답 전달 확인). 안티루프(3회 후 하드페일) 검증도.

---

## 2026-07-15 (P-트랙 20차) — heartbeat·Guardian 큐·보안·다수 견고성 (하루 요약)

하드웨어 반복 테스트로 여러 결함 잡고 Guardian 차별점 첫 조각들 구현. 커밋 분산
(ESP repo `hybrid/w55rp20`, Pico repo `main`). 핵심:

**보안 (완료)**
- 유출된 텔레그램 봇토큰 BotFather 로테이션(사용자) + 하드코딩 제거(dd42e76).
- 근본 유출 채널 차단: `do_http_request`가 매 TG 호출마다 `/bot<token>/`을 시리얼에
  평문 출력하던 것 → `/bot***/`로 마스킹(319f1b3). 이게 로그 붙여넣기마다 토큰 새던 원인.

**heartbeat / 3-state Guardian (구현·검증)**
- ESP `SPI_CMD_ESP_STATUS`를 하드코딩 → 실 health `{seq,up,heap,wifi,proxy}`로(E-3, c2cd63a7).
- Pico가 파싱해 HEALTHY/DEGRADED/HUNG/DEAD 판정 + ESP 리부트 감지(seq/up 회귀)(438a9e2).
  파싱은 Core0로(콜백 경량화, d8358d0). HUNG = STATUS는 오는데 relay 연속 타임아웃 →
  워치독 IC가 못 잡는 케이스(프로그래머블 guardian 정당화).
- HW 검증: esp=HEALTHY, seq 전진, heap 실값, wifi 반영. 정상.

**메시지 무손실 / mid-relay 리부트 (구현)**
- ESP가 groq 답 받은 뒤 LLM_RESP 전 리셋 → 90s 맹목 대기 + 오해성 에러였음.
  → PING epoch로 대기 중 리부트 감지(Core1) → 즉시 정직 응답(e9e24de).
- 메시지-무손실 큐 MVP(0f94559): mid-relay 리셋 시 메시지 보관 → ~35s 후 자동 재질의
  1회(`on_telegram_message` 재사용). Pico RAM 전용(ESP 재부팅 견딤, Pico 재부팅은 v2 flash 큐).
- 정직 메시지 분리(38cc0a3): ESP 도달+에러(ok=false) vs 진짜 무응답 구분 —
  "연결 끊김" 오표기 제거.

**빌드 회귀 (자책·수정)**
- reset_reason 로그 추가하다 `app_allocate_runtime_state()` 호출을 실수로 삭제
  → s_config NULL → `app_config_load` INVALID_ARG 부팅루프. "unused function" 경고가
  증거였는데 오판. 복구(f5ec9732 amend). 교훈: unused-static 경고 무시 금지.
- CRC error 반복은 stale 바이너리(불완전 빌드)였고 클린 빌드로 소멸.

**"리부트" 정체 규명**
- 대부분 `reset_reason=11`(USB) / `rst:0x15` = idf_monitor USB-CDC 재접속이 칩 리셋 →
  디버그 아티팩트, 헤드리스엔 무관. `esp_reset_reason()` 부팅 로그로 확정(monitor가
  ROM 헤더 놓쳐서 앱에서 직접 뽑음).

**발견: esp-claw 코어 이슈 (우리 유선 스코프 밖, 기록만)**
- 툴 검증 400: 모델이 `cap_time`/`cap_web_search` 등 request.tools에 없는 툴 호출 →
  groq 400. 등록된 cap ↔ LLM 노출 tools 불일치.
- **힙 손상 크래시**: `remove_free_block` TLSF assert @ `claw_cap_build_llm_tools_json →
  cJSON_Delete`(claw_cap.c:350). crash 지점 ≠ 버그 지점(이전에 힙 손상, free가 발견).
  매 요청 12970B 툴 JSON 재빌드 부하로 표면화. 코어 힙 버그 → HEAP_POISONING 켜야
  범인 특정 = 별도 upstream 작업.
- 세션 히스토리 무한 누적 + fatfs persist → 리셋해도 주제 고착. 테스트 정상화는
  `parttool erase_partition --partition-name=storage`(NVS 보존, 세션만 클리어).
- **입장**: 이들은 코어 결함 → 우리가 고치는 게 아니라 **Guardian이 감지·복구·완화**하는
  대상. "불완전한 코어 위에서 시스템을 살아있게 유지"가 유선 하이브리드의 가치 증명.

**우리 레이어 상태**: 릴레이/heartbeat/큐/보안 전부 정상, 어떤 crash backtrace에도 안 낌.
유선 경로는 27KB 멀티툴·400·연속요청 다 완벽 왕복. 안정성 이슈는 전부 upstream 코어.

**다음**: (1) 메시지큐 mid-relay 리셋 단독 검증(Pico 생존 유지) (2) EN 리셋선(HUNG/DEAD →
물리 리셋) (3) v2 flash 큐(Pico 재부팅 견딤) (4) 코어 힙버그는 upstream 트랙.

## 2026-07-14 (P-트랙 19차) — 하드웨어 검증: WiFi 완전다운 시나리오 일관 10초대

18차 수정(중복 재시도 스킵) 반영 빌드로 재테스트. 안테나 뽑은 채로 부팅 → STA 5회
재시도 전부 실패 → `STA failed after 5 retries, falling back to AP` → **부팅 시점부터
`g_use_spi_proxy=true`** (WiFi 아예 시도 안 함, on_wifi_state_changed(false)가 부팅
직후 발동).

**결과 — 3회 연속 요청, 전부 성공, 일관된 지연**:
| req | LLM_REQ→LLM_RESP | 
|-----|------------------|
| 1 | 10.7s |
| 2 | 10.1s |
| 3 | 10.4s |

`relay_ok=3, relay_err=0, relay_to=0, local_fb=0, local_fb_unavail=0`. 크래시 0, 힙 안정,
세션 컨텍스트 유지(36→213→369). P-5(esp_alive) PING 즉시 반영 정상.

**주의**: 이 런은 "WiFi가 처음부터 확정적으로 죽음" 케이스라 애초에 WiFi 시도를 안 함
→ **18차가 정확히 노린 "WiFi는 연결됐다가 중간에 리셋되는" 케이스는 이걸로 검증 안 됨**
(14차/18차 원래 재현 조건). 다만 결과가 일관되게 좋아 이 경로는 확정 양호로 판단,
"연결 후 리셋" 케이스는 재현 기회 있을 때 추가 확인.

---

## 2026-07-14 (P-트랙 18차) — 재분석: "WiFi 없음"이 아니라 "약한 신호로 연결 후 리셋" + 중복 재시도 제거

빌드+플래시 후 실측: **안테나를 뽑은 상태**(RSSI -71)로 테스트. 17차 진단을 정정한다 —
DNS/네트워크가 아예 없는 상황이 아니라 **STA는 실제로 연결되고 IP도 받았지만
(`wifi:connected...rssi=-71` → `sta ip: 192.168.11.2`, `WiFi restored, SPI proxy
disabled`) 33초 후 실제 LLM 요청에서 TLS handshake가 리셋됨**
(`mbedtls_ssl_handshake returned -0x0050`). 즉 14차와 같은 계열(드라이버는 연결됨이라
보고하지만 실제 세션이 불안정) — "WiFi가 전혀 없다"는 전제는 틀렸음.

req=1 총 68s 분해: 1차 시도 실패(~4.7s, handshake reset) → 지터 3.7s →
**2차 시도(~14.5s, DNS/connect 재시도 — 17차에서 분석한 getaddrinfo 블로킹 경로)** →
프록시 전환 → 프록시 왕복(~10s) → 컨텍스트 로딩 등 나머지.

**신규 발견 및 수정**: 1차 실패 후의 지터+2차 WiFi 재시도(`claw_llm_http_transport.c`
420번대) 존재 이유는 "claw_core + async-memory-extract 동시 DNS 조회 시 양쪽 다
EAI_AGAIN" 레이스 방지용이라는 주석이 있었음. 근데 **`s_llm_http_mutex`가 이 함수
전체를 이미 직렬화**(382-383줄, SPI 프록시 등록 시에만 생성됨) → 프록시 있는 빌드에서는
저 동시성 레이스 자체가 원천적으로 불가능 → 재시도가 존재할 이유가 없음. **수정**:
`s_spi_proxy_fn` 등록 시 이 재시도를 건너뛰고 곧장 프록시로. 프록시 미등록(순정 WiFi
배포)은 재시도 유지(레이스 방지 여전히 유효).

**예상 효과**: 1차 실패(~4.7s) → 즉시 프록시(~10s) ≈ **15s** (기존 68s 대비 대폭 감소,
17차의 8s 캡보다 이 수정의 효과가 훨씬 큼 — 진짜 병목은 "중복 재시도"였다).

**검증 대기**: 안테나 뺀 채로 재현, req=1 왕복이 15~20s대로 떨어지는지 확인.

---

## 2026-07-14 (P-트랙 17차) — req=2 40s 완화: 프록시 있을 때 WiFi 타임아웃 단축

결함 ③ 재분석. 14차에서 "25s WiFi 타임아웃+재시도"로 추정했으나 실제 로그 재확인 결과
정확한 메커니즘은 다름: `ESP_ERR_HTTP_CONNECT` 재시도(1~3s 지터)가 아니라, **POST 바디
전송 중 `esp_tls_conn_write`가 약 29초간 멈춰 있다가 "Connection reset by peer"로 실패**
→ 그 즉시(재시도 루프 없이) 인라인 SPI 프록시로 전환 → 프록시 자체는 ~11s. 합 ~40s.

`esp_http_client_config_t.timeout_ms`는 **총 요청 데드라인이 아니라 개별 I/O(select())
타임아웃** — 읽기/쓰기가 진행될 때마다 리셋되므로, 정상 연결에서 느린 완성 응답을 기다리는
동안엔 전혀 영향 없다. 끊긴/불량 소켓에서 **한 번의 read/write가 멈출 때만** 발동.
기존 기본값(`CLAW_LLM_DEFAULT_TIMEOUT_MS`, claw_llm_runtime.c, 25s)이 이 케이스의 지연
전부.

**수정** (`claw_llm_http_transport.c`): SPI 프록시가 등록돼 있을 때만
`config.timeout_ms`를 **8초**로 캡 (`CLAW_LLM_HTTP_PROXY_FALLBACK_TIMEOUT_MS`). 이미
동작 확인된 프록시 경로로 더 빨리 넘어가는 게 25s+ 멈춰있는 것보다 항상 낫다는 판단.
프록시 미등록(순정 WiFi 전용 배포)엔 영향 없음 — `s_spi_proxy_fn`이 NULL이면 조건 자체가
성립 안 함.

**예상 효과**: req=2류 케이스 ~40s → ~8s(막힌 WiFi 감지) + ~11s(프록시) ≈ **19s**.

**검증 대기**: 하드웨어 미확인. 다음 빌드에서 시나리오 3(WiFi 절단/복구 반복) 재실행 시
막힌-소켓 케이스가 재현되면 왕복 시간이 19s 부근인지 확인. 부작용 확인 포인트: 정상
WiFi 경로에서 느린 완성 응답(수 초~10초대)이 8s 캡에 걸려 조기 프록시 전환되지 않는지 —
timeout_ms가 진짜 per-I/O인지 실측으로 재확인 필요(문서상 근거는 맞으나 esp-tls 내부
select 적용 방식은 IDF 버전별 차이 가능).

---

## 2026-07-14 (P-트랙 16차) — dead-ESP 감지(P-5) + 로컬 폴백 401 처리 (Pico 코드)

15차에서 확정된 두 결함을 Pico `examples/wiz_claw_spi_host/main.c`에서 수정
(WIZnet-PICO-C-CHIP-TEST repo, `main` 브랜치). 하드웨어 미검증 — 다음 세션에서
시나리오 4(ESP만 다운, Pico 생존 유지) 재현으로 확인 필요.

**P-5 dead-ESP 감지**: `g_esp32_last_seen_ms`를 PING/STATUS 수신마다 갱신.
`check_esp32_alive_timeout()`을 메인 루프에서 매 iteration 호출(TG poll 5s 블로킹이라
실질 ~5s 간격) — 마지막 수신 후 65초(STATUS 20s 주기 3회 누락 + 여유) 지나면
`g_esp32_alive=false`로 되돌림. 이걸로 90s-latch 버그(독립급전 확정, DEVLOG 15차)
해소: ESP가 죽으면 최대 65s 내 감지 → 이후 릴레이 즉시 스킵, 매 메시지 90s 낭비 없음.
복귀는 기존 로직 그대로(PING/STATUS 수신 시 true).

**로컬 폴백 401 처리**: `on_telegram_message`에서 로컬 폴백 진입 전
`g_settings.llm_api_key[0] == '\0'` 체크 추가. 키 없으면 헛방 TLS 호출(항상 401) 대신
즉시 스킵하고 원인을 명시한 메시지 전송("ESP32 연결 끊김 + 로컬 백업 미설정"). 신규
카운터 `local_fb_unavail`을 `[health]` 라인에 추가해 구분(`local_fb`=실제 로컬 LLM
시도, `local_fb_unavail`=키 없어 스킵).

**검증 대기**: 시나리오 4 재현 시 (a) ESP 다운 후 65s 이내 `esp_alive` 0으로 전환되는
`[health]` 라인 확인 (b) 그 이후 메시지가 즉시(수초 내) 응답되는지(로컬 키 있으면
정상 답, 없으면 신규 메시지) — 90s 안 걸리는지가 핵심 판정 기준.

---

## 2026-07-13 (S-트랙 15차) — 시나리오 4 (ESP 다운): 90s-latch 미발생, 로컬 폴백 결함 노출

ESP USB 뽑아 다운시킴. **관찰: Pico도 같이 리부트됨**(`cfg flash empty→defaults`,
`esp_alive=0` 시작). → ESP 급전 공유 의심.

**동작**:
- ESP 부재 중 "죽음2/3": Pico `esp_alive=0` → 릴레이 **건너뜀** → 즉시 로컬 폴백 →
  로컬 Groq **401**(Pico 로컬 LLM 키 미설정) → 유저에 "죄송합니다 오류". relay_to=0, local_fb=2.
  → **90s 낭비 없음**(esp_alive=false면 릴레이 스킵이 올바르게 동작).
- ESP 재부팅 → PING → esp_alive=1 → "복구1" 릴레이 정상 복귀. 세션 컨텍스트도 유지
  (context_len 702).

**결론 정정**:
- **P-5(dead-ESP 감지) 재평가 필요.** 90s-latch 버그(`esp_alive` false 복귀 없음)는
  "ESP만 죽고 Pico 생존" 경우만 터짐. 이번엔 Pico도 리부트 → 플래그 리셋 → 미발생.
  전원 구성이 공유(ESP 급전)면 항상 동반 리부트 → 버그 사실상 안 터짐 → P-5 우선순위↓.
  독립 급전이면 실재 → 재테스트(Pico 유지한 채 ESP만 다운) 필요.
  **[확정: 독립 급전]** → 90s-latch 버그 실재. P-5 필요(STATUS 20s 미수신 N회 →
  esp_alive=false + 히스테리시스). 단 이번 런에선 독립 급전임에도 Pico가 동반 리부트됨
  (`cfg flash empty`, up=27s) → ESP 전원 차단 시 SPI 라인 글리치/공유 GND 교란/시리얼 툴
  리셋 의심(Pico watchdog 미탑재라 원인 불명). 별도 관찰 포인트. 정밀 재현 시 Pico `up=`이
  계속 증가하는지(생존) 확인하면서 ESP만 다운.
- **신규 결함 — 로컬 폴백 비작동(401).** Pico 로컬 LLM 키 미설정으로 ESP 다운 시 폴백이
  항상 실패→유저 에러. 선택: (a) Pico 웹서버(192.168.11.20)에서 로컬 키 설정해 폴백 활성,
  (b) 폴백 무의미하면 "서비스 불가" 깔끔한 메시지로. esp-claw 정식 대안 포지션에선 (a)가 맞음
  (유선 노드가 WiFi 없이도 자립).

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
