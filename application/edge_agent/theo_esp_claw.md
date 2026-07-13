# ESP-CLAW Edge Agent - 보드 설정 및 빌드 가이드

## 보드 설정 및 빌드 순서

### 1단계: 보드 코드 생성 (최초 1회)
```powershell
idf.py bmgr -b esp32_S3_DevKitC_1 -c boards/espressif
```
→ `components/gen_bmgr_codes/` 생성됨

---

### 2단계: `boards/espressif/esp32_S3_DevKitC_1/sdkconfig.defaults.board` 수정
`CONFIG_APP_CLAW_LUA_MODULE_LCD=y` 제거

- DevKitC-1은 LCD 없음
- 해당 설정이 있으면 IDF v6.0과 비호환되는 LCD 드라이버(`esp_lcd_gc9107`, `esp_lcd_gc9d01`)가 빌드에 포함되어 컴파일 에러 발생

---

### 3단계: bmgr 재실행 (sdkconfig.defaults.board 수정 후 반영)
```powershell
idf.py bmgr -b esp32_S3_DevKitC_1 -c boards/espressif
```

---

### 4단계: sdkconfig 초기화 및 빌드
```powershell
Remove-Item sdkconfig
idf.py build
```

---

### 5단계: 플래싱
```powershell
idf.py -p COM36 flash
```

---

### 6단계: 모니터
```powershell
idf.py -p COM36 monitor
```

모니터 종료: `Ctrl+]`

---

## 보충 사항

### VS Code 파일 잠금 방지
`.vscode/settings.json`에 아래 설정 추가 → `managed_components`, `build` 폴더 감시 제외

```json
"files.watcherExclude": {
  "**/managed_components/**": true,
  "**/build/**": true
},
"files.exclude": {
  "**/managed_components": true,
  "**/build": true
}
```

### 보드 변경 시
**1단계 → 3단계 → 4단계** 순서 반복

### 프로비저닝 포털 접속
1. PC/폰의 WiFi를 **`esp-claw-XXXXXX`** AP로 전환
2. 브라우저에서 `http://192.168.4.1/` 접속
3. WiFi SSID/비밀번호 + LLM API 키 입력
4. 이후 `http://esp-claw.local/` 로 접속 가능
