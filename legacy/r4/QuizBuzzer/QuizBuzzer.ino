/*
 * Quiz Buzzer System - Arduino UNO R4 WiFi
 * 
 * Supports 4 players with illuminated buttons.
 * BLE controlled via Web Bluetooth API.
 * 
 * Wiring (per player):
 *   Button signal  → Digital pin (2, 4, 6, 8)
 *   Button LED     → Digital pin (3, 5, 7, 9) via 220Ω resistor
 *   Button GND     → GND
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
const int BUTTON_PINS[4] = {2, 4, 6, 8};
const int LED_PINS[4]    = {3, 5, 7, 9};
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
// Status values: 0 = idle, 1 = round active (flashing), 2 = buzzed
enum GameStatus { IDLE = 0, ROUND_ACTIVE = 1, BUZZED = 2 };
GameStatus gameStatus = IDLE;
int buzzedPlayer = 0;       // 0 = none, 1-4 = player number

// --- Flash animation state ---
unsigned long lastFlashTime = 0;
bool flashState = false;
const int FLASH_INTERVAL_MS = 300;

// --- Button debounce ---
bool lastButtonState[4] = {HIGH, HIGH, HIGH, HIGH};
unsigned long lastDebounceTime[4] = {0, 0, 0, 0};
const int DEBOUNCE_DELAY = 50;

void setup() {
  Serial.begin(9600);

  // Configure pins
  for (int i = 0; i < NUM_PLAYERS; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }

  // Startup LED test — all LEDs on briefly
  for (int i = 0; i < NUM_PLAYERS; i++) digitalWrite(LED_PINS[i], HIGH);
  delay(500);
  for (int i = 0; i < NUM_PLAYERS; i++) digitalWrite(LED_PINS[i], LOW);

  // --- BLE init ---
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

  // Initial values
  buzzerStateChar.writeValue(0);
  statusChar.writeValue(IDLE);

  BLE.advertise();
  Serial.println("QuizBuzzer ready, advertising BLE...");
}

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

    Serial.println("Disconnected");
    // Reset to idle on disconnect
    setIdle();
  }
}

// --- Handle incoming BLE writes ---
void handleBLECommands() {
  // Reset command
  if (resetChar.written()) {
    Serial.println("Reset received");
    setIdle();
  }

  // Round start command
  if (roundStartChar.written()) {
    Serial.println("Round start received");
    setRoundActive();
  }
}

// --- Handle physical button presses ---
void handleButtons() {
  if (gameStatus != ROUND_ACTIVE) return;  // Only accept buzzes during active round

  for (int i = 0; i < NUM_PLAYERS; i++) {
    int reading = digitalRead(BUTTON_PINS[i]);

    // Debounce
    if (reading != lastButtonState[i]) {
      lastDebounceTime[i] = millis();
    }

    if ((millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY) {
      // Button pressed (LOW because INPUT_PULLUP)
      if (reading == LOW && lastButtonState[i] == HIGH) {
        Serial.print("Player ");
        Serial.print(i + 1);
        Serial.println(" buzzed!");
        setBuzzed(i + 1);
        break;  // Lockout — only first player counts
      }
    }

    lastButtonState[i] = reading;
  }
}

// --- LED update logic ---
void updateLEDs() {
  unsigned long now = millis();

  switch (gameStatus) {
    case IDLE:
      // All LEDs off
      for (int i = 0; i < NUM_PLAYERS; i++) digitalWrite(LED_PINS[i], LOW);
      break;

    case ROUND_ACTIVE:
      // All LEDs flash in sync
      if (now - lastFlashTime >= FLASH_INTERVAL_MS) {
        flashState = !flashState;
        lastFlashTime = now;
        for (int i = 0; i < NUM_PLAYERS; i++) {
          digitalWrite(LED_PINS[i], flashState ? HIGH : LOW);
        }
      }
      break;

    case BUZZED:
      // Only buzzed player's LED stays solid on; others off
      for (int i = 0; i < NUM_PLAYERS; i++) {
        digitalWrite(LED_PINS[i], (i == buzzedPlayer - 1) ? HIGH : LOW);
      }
      break;
  }
}

// --- State transition helpers ---
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
