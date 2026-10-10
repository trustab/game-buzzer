/*
 * Quiz Buzzer System - Arduino UNO R4 WiFi
 * Modular I²C version using PCF8574 GPIO expanders
 *
 * Each button module contains a PCF8574 and connects via 4-wire I²C bus:
 *   GND, 5V, SDA (A4), SCL (A5)
 *
 * PCF8574 I²C addresses (set via A0/A1/A2 solder jumpers on each module):
 *   Module 1 (Player 1): 0x20  (A2=0, A1=0, A0=0)
 *   Module 2 (Player 2): 0x21  (A2=0, A1=0, A0=1)
 *   Module 3 (Player 3): 0x22  (A2=0, A1=1, A0=0)
 *   Module 4 (Player 4): 0x23  (A2=0, A1=1, A0=1)
 *
 * PCF8574 pin usage (per module):
 *   P0 — Button signal (INPUT, uses PCF8574 internal weak pull-up)
 *   P1 — LED control (OUTPUT, active LOW — sink current through LED)
 *   P2-P7 — unused
 *
 * LED wiring per module:
 *   5V → 220Ω resistor → LED anode → LED cathode → PCF8574 P1
 *   (PCF8574 sinks current when P1 is LOW)
 *
 * BLE Service UUID:     12345678-1234-1234-1234-123456789abc
 * Characteristics:
 *   Buzzer State UUID:  12345678-1234-1234-1234-123456789ab1  (notify/read)
 *   Reset UUID:         12345678-1234-1234-1234-123456789ab2  (write)
 *   Round Start UUID:   12345678-1234-1234-1234-123456789ab3  (write)
 *   Status UUID:        12345678-1234-1234-1234-123456789ab4  (notify/read)
 */

#include <Wire.h>
#include <ArduinoBLE.h>

// --- I²C addresses for each player module ---
const uint8_t MODULE_ADDR[4] = { 0x20, 0x21, 0x22, 0x23 };
const int NUM_PLAYERS = 4;

// PCF8574 pin masks
const uint8_t PIN_BUTTON = 0b00000001;  // P0 — button input
const uint8_t PIN_LED    = 0b00000010;  // P1 — LED output (active LOW)

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

// --- Button debounce ---
bool lastButtonState[4]       = { true, true, true, true }; // true = not pressed (pull-up)
unsigned long lastDebounceTime[4] = { 0, 0, 0, 0 };
const int DEBOUNCE_DELAY = 50;

// --- Module presence tracking ---
bool modulePresent[4] = { false, false, false, false };

// ─────────────────────────────────────────────
//  PCF8574 helpers
// ─────────────────────────────────────────────

// Write a full byte to the PCF8574.
// PCF8574 outputs are open-drain: writing 1 = input/high-Z (pulled high),
// writing 0 = actively pulled LOW.
void pcfWrite(uint8_t addr, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(value);
  Wire.endTransmission();
}

// Read a full byte from the PCF8574.
// Pins to be used as inputs must be written HIGH first to enable the pull-up.
uint8_t pcfRead(uint8_t addr) {
  Wire.requestFrom(addr, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0xFF; // default high (unpressed)
}

// Set LED state for a module (ledOn=true → LED on)
// LED is active LOW on P1
void setLED(int player, bool ledOn) {
  if (!modulePresent[player]) return;
  // P0 high (button input), P1: LOW=LED on, HIGH=LED off
  uint8_t val = PIN_BUTTON | (ledOn ? 0x00 : PIN_LED);
  pcfWrite(MODULE_ADDR[player], val);
}

// Read button state for a module (returns true if pressed)
bool readButton(int player) {
  if (!modulePresent[player]) return false;
  uint8_t val = pcfRead(MODULE_ADDR[player]);
  return !(val & PIN_BUTTON); // active LOW
}

// ─────────────────────────────────────────────
//  I²C bus scan — detect which modules are present
// ─────────────────────────────────────────────
void scanModules() {
  Serial.println("Scanning I2C bus for modules...");
  for (int i = 0; i < NUM_PLAYERS; i++) {
    Wire.beginTransmission(MODULE_ADDR[i]);
    uint8_t err = Wire.endTransmission();
    modulePresent[i] = (err == 0);
    Serial.print("  Module ");
    Serial.print(i + 1);
    Serial.print(" (0x");
    Serial.print(MODULE_ADDR[i], HEX);
    Serial.print("): ");
    Serial.println(modulePresent[i] ? "FOUND" : "not found");

    if (modulePresent[i]) {
      // Set P0 high (input mode), P1 high (LED off)
      pcfWrite(MODULE_ADDR[i], PIN_BUTTON | PIN_LED);
    }
  }
}

// ─────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  Wire.begin();
  scanModules();

  // Startup LED test
  for (int i = 0; i < NUM_PLAYERS; i++) setLED(i, true);
  delay(500);
  for (int i = 0; i < NUM_PLAYERS; i++) setLED(i, false);

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

    Serial.println("Disconnected");
    setIdle();
  }
}

// ─────────────────────────────────────────────
//  BLE command handler
// ─────────────────────────────────────────────
void handleBLECommands() {
  if (resetChar.written())     { setIdle(); }
  if (roundStartChar.written()) { setRoundActive(); }
}

// ─────────────────────────────────────────────
//  Button handler
// ─────────────────────────────────────────────
void handleButtons() {
  if (gameStatus != ROUND_ACTIVE) return;

  for (int i = 0; i < NUM_PLAYERS; i++) {
    if (!modulePresent[i]) continue;

    bool pressed = readButton(i);

    if (pressed != lastButtonState[i]) {
      lastDebounceTime[i] = millis();
    }

    if ((millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY) {
      if (pressed && !lastButtonState[i]) {
        Serial.print("Player ");
        Serial.print(i + 1);
        Serial.println(" buzzed!");
        setBuzzed(i + 1);
        break; // lockout
      }
    }

    lastButtonState[i] = pressed;
  }
}

// ─────────────────────────────────────────────
//  LED update
// ─────────────────────────────────────────────
void updateLEDs() {
  unsigned long now = millis();

  switch (gameStatus) {
    case IDLE:
      for (int i = 0; i < NUM_PLAYERS; i++) setLED(i, false);
      break;

    case ROUND_ACTIVE:
      if (now - lastFlashTime >= FLASH_INTERVAL_MS) {
        flashState = !flashState;
        lastFlashTime = now;
        for (int i = 0; i < NUM_PLAYERS; i++) setLED(i, flashState);
      }
      break;

    case BUZZED:
      for (int i = 0; i < NUM_PLAYERS; i++) {
        setLED(i, (i == buzzedPlayer - 1));
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
