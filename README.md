# Quiz Buzzer

Wireless quiz buzzers: ESP32-C3 button units talk ESP-NOW to an ESP32-C3 hub, which decides who pressed first and reports to a web page over Web Bluetooth.

```
[Button ESP32-C3] ─┐
[Button ESP32-C3] ─┼── ESP-NOW ──► [Hub ESP32-C3] ── BLE ──► Web page (Chrome)
[Button ESP32-C3] ─┘
```

## Web page
Open the GitHub Pages site in Chrome or Edge (desktop or Android) and click Connect. Web Bluetooth needs HTTPS or localhost, so to run without internet:

```
npx serve .
```

## Flashing
Uses arduino-cli (on PATH, or the copy bundled with Arduino IDE 2).

```powershell
.\tools\flash.ps1 -Setup                       # once: ESP32 core + NimBLE-Arduino
.\tools\flash.ps1 hub -Port COM4 -Monitor
.\tools\flash.ps1 button -Player 2 -Port COM5
```

Each button is built with its own player number (1–4). Run without `-Port` to list connected boards.

See `CLAUDE.md` for hardware, protocol and test plan details.
