// buzzer_button.ino — PoC button unit: one button, one LED, 3xAAA, ESP-NOW to hub.
// Board: ESP32-C3 SuperMini ("ESP32C3 Dev Module", USB CDC On Boot: Enabled)
// No extra libraries needed.
//
// Wiring:
//   3xAAA + -> 5V pin        3xAAA - -> GND
//   Button  -> GPIO3 and GND (internal pull-up)
//   LED     -> GPIO5 -> 150 ohm -> LED anode, LED cathode -> GND
//   Battery divider (not fitted yet): BAT+ -> 100k -> BATT_PIN -> 100k -> GND

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "protocol.h"

#ifndef PLAYER_ID
#define PLAYER_ID   1     // per button: 1-4, or set by tools/flash.ps1 -Player N
#endif
#define BUTTON_PIN  3
#define LED_PIN     5
// #define BATT_PIN 4     // ADC pin for the battery divider; leave undefined until fitted

#define HELLO_MS        5000   // battery heartbeat interval
#define HUB_TIMEOUT_MS  2000   // no STATE heard for this long = hub lost
#define PRESS_RETRIES   5      // broadcast has no ACK, so resend a few times
#define RETRY_MS        20

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Written in the WiFi task, read in loop()
volatile uint8_t  hubState  = STATE_IDLE;
volatile uint8_t  hubWinner = 0;
volatile uint16_t hubRound  = 0;
volatile uint32_t lastHeard = 0;

uint16_t pressedRound = 0;     // round we've already pressed in
bool     armed = false;        // button must be released after round start (no holding it down)
uint8_t  retriesLeft = 0;
uint32_t lastRetry = 0;
uint32_t lastHello = 0;

void onEspNowRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) {
  if (len != sizeof(BuzzMsg)) return;
  const BuzzMsg* m = (const BuzzMsg*)data;
  if (m->magic != BUZZ_MAGIC || m->type != MSG_STATE) return;
  hubState  = m->state;
  hubWinner = m->player;
  hubRound  = m->round;
  lastHeard = millis();
}

// Returns 0 without a divider; the hub treats that as "no battery" rather than low.
uint16_t readBatteryMv() {
#ifdef BATT_PIN
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(BATT_PIN);
  return (sum / 8) * 2;   // divider halves the voltage
#else
  return 0;
#endif
}

void send(MsgType type, uint16_t round, uint16_t battMv) {
  BuzzMsg m = {BUZZ_MAGIC, (uint8_t)type, PLAYER_ID, 0, battMv, round};
  esp_now_send(BROADCAST, (const uint8_t*)&m, sizeof(m));
}

void updateLed() {
  bool on;
  bool hubAlive = millis() - lastHeard < HUB_TIMEOUT_MS;
  if (!hubAlive) {
    on = (millis() % 2000) < 50;                   // short blip = no hub
  } else if (hubState == STATE_ACTIVE) {
    on = (millis() / 125) % 2;                     // flash = round running
  } else if (hubState == STATE_BUZZED) {
    on = (hubWinner == PLAYER_ID);                 // solid = you won
  } else {
    on = false;
  }
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
#ifdef BATT_PIN
  analogSetAttenuation(ADC_11db);
#endif

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(BUZZ_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);   // keep the radio awake so we hear round starts instantly
  // Lower TX power: smaller current spikes (AAA cells sag under load) and it
  // often works better on SuperMini clones with a poor antenna match.
  // Plenty for a room; raise it if presses get lost at a distance.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.printf("Button %d ready, MAC %s\n", PLAYER_ID, WiFi.macAddress().c_str());
}

void loop() {
  uint8_t  state = hubState;
  uint16_t round = hubRound;
  bool down = digitalRead(BUTTON_PIN) == LOW;

  // Arm only once the button has been seen released during this round,
  // so holding it down before the start doesn't count.
  if (state != STATE_ACTIVE) armed = false;
  else if (!down)            armed = true;

  // No debounce needed: the first LOW sends the press and pressedRound
  // blocks anything else until the hub starts a new round.
  if (state == STATE_ACTIVE && armed && down && pressedRound != round) {
    pressedRound = round;
    send(MSG_PRESS, round, 0);
    retriesLeft = PRESS_RETRIES - 1;
    lastRetry = millis();
    Serial.printf("PRESS sent (round %u)\n", round);
  }

  // Resend until the hub leaves ACTIVE (it got a press from someone) or retries run out.
  if (retriesLeft && millis() - lastRetry >= RETRY_MS) {
    if (hubState == STATE_ACTIVE) send(MSG_PRESS, pressedRound, 0);
    retriesLeft = (hubState == STATE_ACTIVE) ? retriesLeft - 1 : 0;
    lastRetry = millis();
  }

  if (millis() - lastHello > HELLO_MS) {
    uint16_t mv = readBatteryMv();
    send(MSG_HELLO, round, mv);
#ifdef BATT_PIN
    Serial.printf("Battery %.2f V\n", mv / 1000.0);
#endif
    lastHello = millis();
  }

  updateLed();
}
