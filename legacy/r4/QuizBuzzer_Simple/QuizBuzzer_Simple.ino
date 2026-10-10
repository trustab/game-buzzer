/*
 * Quiz Buzzer System - Arduino UNO R4 WiFi
 * Simple direct-wired version — no I²C, no expander chips
 *
 * Each button module contains only:
 *   - Illuminated momentary push button
 *   - JST XH 3-pin socket
 *
 * All wiring in main unit:
 *   Pin 1 (signal) → Arduino digital input  (D2, D4, D6, D8)
 *   Pin 2 (LED+)   → 220Ω resistor → Arduino PWM output (D3, D5, D7, D9)
 *   Pin 3 (GND)    → GND
 *
 * INPUT_PULLUP used on signal pins:
 *   - Button pressed  = pin reads LOW
 *   - Button released = pin reads HIGH
 *   - Cable unplugged = pin reads HIGH (treated as not pressed, safely ignored)
 *
 * BLE Service UUID:     12345678-1234-1234-1234-123456789abc
 * Characteristics:
 *   Buzzer State UUID:  12345678-1234-1234-1234-123456789ab1  (notify/read)
 *   Reset UUID:         12345678-1234-1234-1234-123456789ab2  (write)
 *   Round Start UUID:   12345678-1234-1234-1234-123456789ab3  (write)
 *   Status UUID:        12345678-1234-1234-1234-123456789ab4  (notify/read)
 */

#include <ArduinoBLE.h>

// --- Pin definitions ---
const int BUTTON_PINS[4] = {2, 4, 6, 8};   // Signal pins (INPUT_PULLUP)
const int LED_PINS[4]    = {3, 5, 7, 9};   // LED pins via 220Ω resistor
const int NUM_PLAYERS    = 4;

// --- BLE UUIDs ---
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define BUZZER_STATE_UUID   "12345678-1234-1234-1234-123456789ab1"
#define RESET_UUID          "12345678-1234-1234-1234-123456789ab2"
#define ROUND_START_UUID    "12345678-1234-1234-1234-123456789ab3"
#define STATUS_UUID         "12345678-1234-1234-1234-123456789ab4"

// --- BLE objects ---
BLEService buzzerService(SERVICE_UUID);
BLEByteCharacteristic buzzerStateChar(BUZZER_STATE_UUID, BLERead | BLENotify);
BLEByteCharacteristic resetChar(RESET_UUID,              BLEWrite);
BLEByteCharacteristic roundStartChar(ROUND_START_UUID,   BLEWrite);
BLEByteCharacteristic statusChar(STATUS_UUID,            BLERead | BLENotify);

// --- Game state ---
enum GameStatus { IDLE = 0, ROUND_ACTIVE = 1, BUZZED = 2 };
GameStatus gameStatus = IDLE;
int buzzedPlayer = 0;

// --- Flash animation ---
unsigned long lastFlashTime = 0;
bool flashState = false;
const int FLASH_INTERVAL_MS = 300;




// --- BLE advertising watchdog ---
unsigned long lastAdvertiseCheck  = 0;
const int ADVERTISE_CHECK_MS      = 5000;  // recheck every 5 seconds

// ─────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  for (int i = 0; i < NUM_PLAYERS; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }

  // Startup LED test
  for (int i = 0; i < NUM_PLAYERS; i++) digitalWrite(LED_PINS[i], HIGH);
  delay(500);
  for (int i = 0; i < NUM_PLAYERS; i++) digitalWrite(LED_PINS[i], LOW);

  // BLE setup
  if (!BLE.begin()) {
    Serial.println("BLE init failed!");
    while (1);
  }

  BLE.setLocalName("QuizBuzzer");
  BLE.setAdvertisedService(buzzerService);
  buzzerService.addCharacteristic(buzzerStateChar);
  buzzerService.addCharacteristic(resetChar);
  buzzerService.addCharacteristic(roundStartChar);
  buzzerService.addCharacteristic(statusChar);
  BLE.addService(buzzerService);

  buzzerStateChar.writeValue(0);
  statusChar.writeValue((byte)IDLE);

  BLE.advertise();
  Serial.println("QuizBuzzer ready.");
}

// ─────────────────────────────────────────────
//  Main loop
// ─────────────────────────────────────────────
void loop() {
  BLEDevice central = BLE.central();

  if (central) {
    Serial.print("Connected: ");
    Serial.println(central.address());

    while (central.connected()) {
      handleBLECommands();
      handleButtons();
      updateLEDs();
    }

    // Central disconnected — reset state and restart advertising.
    // The 500ms delay gives the OS time to fully release the connection
    // handle before we advertise again, which prevents the device from
    // disappearing from Chrome's scan list on reconnect attempts.
    Serial.println("Disconnected — restarting advertise...");
    setIdle();
    BLE.stopAdvertise();
    delay(500);
    BLE.advertise();
    Serial.println("Advertising");

  } else {
    // Not connected — watchdog: periodically bounce the advertiser to
    // unstick the ESP32 co-processor if it has silently stopped advertising.
    if (millis() - lastAdvertiseCheck > ADVERTISE_CHECK_MS) {
      lastAdvertiseCheck = millis();
      BLE.stopAdvertise();
      delay(50);
      BLE.advertise();
    }
  }
}

// ─────────────────────────────────────────────
//  BLE command handler
// ─────────────────────────────────────────────
void handleBLECommands() {
  if (resetChar.written())      setIdle();
  if (roundStartChar.written()) setRoundActive();
}

// ─────────────────────────────────────────────
//  Button handler — immediate trigger
//  Registers instantly on first LOW reading.
//  Double-trigger is impossible since gameStatus
//  changes to BUZZED on first press, causing all
//  subsequent handleButtons() calls to return early.
// ─────────────────────────────────────────────
void handleButtons() {
  if (gameStatus != ROUND_ACTIVE) return;

  for (int i = 0; i < NUM_PLAYERS; i++) {
    if (digitalRead(BUTTON_PINS[i]) == LOW) {
      Serial.print("Player ");
      Serial.print(i + 1);
      Serial.println(" buzzed!");
      setBuzzed(i + 1);
      break;
    }
  }
}

// ─────────────────────────────────────────────
//  LED update
// ─────────────────────────────────────────────
void updateLEDs() {
  unsigned long now = millis();

  switch (gameStatus) {
    case IDLE:
      for (int i = 0; i < NUM_PLAYERS; i++) digitalWrite(LED_PINS[i], LOW);
      break;

    case ROUND_ACTIVE:
      if (now - lastFlashTime >= FLASH_INTERVAL_MS) {
        flashState = !flashState;
        lastFlashTime = now;
        for (int i = 0; i < NUM_PLAYERS; i++) {
          digitalWrite(LED_PINS[i], flashState ? HIGH : LOW);
        }
      }
      break;

    case BUZZED:
      // Winner LED solid on, all others off
      for (int i = 0; i < NUM_PLAYERS; i++) {
        digitalWrite(LED_PINS[i], (i == buzzedPlayer - 1) ? HIGH : LOW);
      }
      break;
  }
}

// ─────────────────────────────────────────────
//  State transitions
// ─────────────────────────────────────────────
void setIdle() {
  gameStatus = IDLE;
  buzzedPlayer = 0;
  buzzerStateChar.writeValue((byte)0);
  statusChar.writeValue((byte)IDLE);
  Serial.println("State: IDLE");
}

void setRoundActive() {
  gameStatus = ROUND_ACTIVE;
  buzzedPlayer = 0;
  buzzerStateChar.writeValue((byte)0);
  statusChar.writeValue((byte)ROUND_ACTIVE);
  flashState = false;
  lastFlashTime = millis();
  Serial.println("State: ROUND_ACTIVE");
}

void setBuzzed(int player) {
  gameStatus = BUZZED;
  buzzedPlayer = player;
  buzzerStateChar.writeValue((byte)player);
  statusChar.writeValue((byte)BUZZED);
  Serial.print("State: BUZZED by player ");
  Serial.println(player);
}
