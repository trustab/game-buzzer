# Game buzzer

Wireless quiz buzzer system. Each player has a battery-powered button unit. A hub decides who pressed first and reports to a web page in Chrome via Web Bluetooth.

```
[Button ESP32-C3] ─┐
[Button ESP32-C3] ─┼── ESP-NOW ──► [Hub ESP32-C3] ── BLE ──► Web page (Chrome, Web Bluetooth)
[Button ESP32-C3] ─┘
```

## History (short)
- Started as an Arduino UNO R4 WiFi with wired buttons and BLE to a React page. That worked, then we moved to wireless ESP-NOW buttons because cables and connectors cost more than an ESP32 per button.
- The hub stays as the single authority for "first press": letting buttons talk BLE to the browser directly can give the wrong winner (notification order isn't guaranteed) and Chrome handles many BLE connections badly.
- Lessons from the R4 version: no debounce needed for the press itself, since the state machine blocks further presses until the next round. Polling must be fast enough to catch very quick taps.

## Layout
- `buzzer_hub/` — hub sketch (`buzzer_hub.ino` + `protocol.h`)
- `buzzer_button/` — button sketch (`buzzer_button.ino` + `protocol.h`)
- `protocol.h` exists in both folders and must stay identical (Arduino needs it in the sketch folder).
- `web/buzzer-controller-react.html` — current web page. Single-file React (React + Babel from CDN), localStorage for names/scores, New Game button. Must be served over HTTPS (e.g. GitHub Pages) or localhost for Web Bluetooth.
- `index.html` — redirects to the web page, so GitHub Pages (served from repo root, `.nojekyll`) has a short URL.
- `tools/flash.ps1` — compile/flash via arduino-cli (PATH or Arduino IDE's bundled copy). `-Setup` installs ESP32 core + NimBLE. Button builds pass `-DPLAYER_ID=N`; the sketch's `#define` is only the default.
- `docs/` — SVG schematics from the earlier design work. `battery-monitor-schematic.svg` and `bracket-diagram.svg` still apply; the others are for the wired R4 design.
- `legacy/` — wired UNO R4 WiFi era, kept for reference only:
  - `r4/QuizBuzzer_Simple/` — last working R4 sketch (direct-wired, BLE advertising watchdog, debounce fixes). Same BLE contract as the hub.
  - `r4/QuizBuzzer/`, `r4/QuizBuzzer_I2C/` — earlier variants (first version; PCF8574 I2C daisy-chain idea, abandoned).
  - `buzzer-controller.html` — first non-React version of the page.

## Toolchain
- Note: the device will appear in Chrome's picker labelled "Pair", but Web Bluetooth only connects; no OS-level pairing is needed.
- Arduino IDE / arduino-cli, ESP32 core 3.x (`esp32:esp32`), board "ESP32C3 Dev Module", **USB CDC On Boot: Enabled** (otherwise no Serial output).
- Hub needs NimBLE-Arduino 2.x and U8g2. Button needs no extra libraries. First U8g2 build is slow (compiles all fonts); later builds are cached.
- Hub flash usage is ~93% of the default 1.2 MB app partition. If it overflows, switch to a bigger partition scheme (e.g. `PartitionScheme=huge_app` in the FQBN; no OTA used).
- Hub compiles and runs (flashed and tested over serial). Button sketch not yet compiled.

## Hardware
**Button (PoC: 1 unit)** — black ESP32-C3 SuperMini (V1601).
- Power: 3xAA → 5V pin, − → GND.
- Button GPIO4 → GND (INPUT_PULLUP). LED GPIO5 → 150 Ω → LED → GND.
- Battery divider: BAT+ → 100k → GPIO3 → 100k → GND (optional 100 nF GPIO3 → GND). Sketch multiplies by 2.
- Regulator marked "LLVB", SOT-23-5 (low-dropout class), so 3xAA is OK. Expected cutoff around 3.5 V pack (~1.15–1.2 V/cell). Not yet confirmed by test.
- Many SuperMini clones have a poor antenna; if packets drop, try `WiFi.setTxPower(WIFI_POWER_8_5dBm)`.

**Hub** — purple ESP32-C3 board with 0.42" OLED, powered by USB.
- Onboard LED on GPIO8 (polarity assumed active LOW; flip in `updateLed()` if wrong).
- OLED is I2C, SDA GPIO5 / SCL GPIO6 (assumed), SSD1306 72x40, driven with U8g2 `SSD1306_72X40_ER`. Shows: web link + round (top), READY / timer / winner (middle), per-player online + low-battery cells (bottom).
- Two diodes (A7) in its power path, so not suitable for battery power. Fine on USB.

## Protocols
**ESP-NOW** (see `protocol.h`): broadcast in both directions, WiFi channel 1, magic byte `0xB7`, packed `BuzzMsg {magic, type, player, state, battMv, round}`.
- `MSG_PRESS` button → hub, `MSG_HELLO` button → hub every 5 s with battery mV, `MSG_STATE` hub → all.
- Hub broadcasts state every 500 ms and 3x on every change. Button resends a press up to 5x at 20 ms until the hub leaves ACTIVE.
- Presses are tagged with the round number; the hub ignores presses from older rounds.
- Button must be released after round start before a press counts (no holding it down).
- Hub processes ESP-NOW and BLE events through one FreeRTOS queue in `loop()`; queue order decides the winner.
- Hub must not set `WIFI_PS_NONE` (not allowed with BLE active). The button sets it so it hears round starts immediately.

**BLE** (must stay compatible with the web page):
- Device name `QuizBuzzer` (name only in the advert; a 128-bit UUID doesn't fit alongside it).
- Service `12345678-1234-1234-1234-123456789abc`
- `...ab1` buzzer state — read/notify, uint8 player 1–4 (page only reacts to > 0)
- `...ab2` reset — write
- `...ab3` round start — write
- `...ab4` status — read/notify, uint8: 0 idle, 1 active, 2 buzzed

**LED behaviour**
- Button: short blip every 2 s = no hub, fast flash = round running, solid = this button won.
- Hub: short blip = waiting for browser, fast flash = round running, solid = someone buzzed.

## PoC test plan
1. Flash hub, open Serial Monitor, send `s` (start) / `r` (reset) to test without the browser.
2. Button on USB first: LED flashes on start, hub prints `player 1 buzzed first`, battery + RSSI every 5 s.
3. Button on 3xAA: watch for resets (LED pattern restarting) when pressing.
4. Connect from the web page in Chrome.

## Open issues / next steps
- Button radio stays awake (~80–100 mA, about 15–20 h on 3xAA). No deep sleep while a game runs, since sleeping buttons can't hear round starts.
- Pairing: idea is long press (~10 s) to enter pairing mode, then register MACs on the hub instead of open broadcast.
- Send battery level per player over BLE and show green/yellow/red on the web page (low warning near cutoff).
- Support more than 4 players (ESP-NOW handles up to ~20 peers; web page is hardcoded for 4).
- 3D-printed board bracket: slide-in rails with a snap lip; board is about 20x25x1 mm.
