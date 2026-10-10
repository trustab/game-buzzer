// buzzer_hub.ino — PoC hub: ESP-NOW from buttons, BLE to the existing web page.
// Board: ESP32-C3 SuperMini ("ESP32C3 Dev Module", USB CDC On Boot: Enabled)
// Libraries: NimBLE-Arduino 2.x, U8g2 (0.42" 72x40 OLED)
//
// BLE side is identical to the old UNO R4 sketch (name "QuizBuzzer", same UUIDs,
// same byte values), so the React page works unchanged.

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <NimBLEDevice.h>
#include <U8g2lib.h>
#include "protocol.h"

// ── BLE UUIDs (same as the web page) ──────────────────────────────
#define SERVICE_UUID      "12345678-1234-1234-1234-123456789abc"
#define BUZZER_STATE_UUID "12345678-1234-1234-1234-123456789ab1"
#define RESET_UUID        "12345678-1234-1234-1234-123456789ab2"
#define ROUND_START_UUID  "12345678-1234-1234-1234-123456789ab3"
#define STATUS_UUID       "12345678-1234-1234-1234-123456789ab4"
#define PLAYERS_UUID      "12345678-1234-1234-1234-123456789ab5"

#define STATUS_LED 8            // SuperMini onboard LED, active LOW
#define STATE_BROADCAST_MS 500  // heartbeat so buttons always know the state

#define OLED_SDA 5
#define OLED_SCL 6
#define MAX_PLAYERS       4
#define PLAYER_TIMEOUT_MS 12000   // HELLO every 5 s; two missed = offline
#define LOW_BATT_MV       3600    // 3xAA; regulator drops out around 3.5 V
#define NO_BATT_MV        1000    // below this: on USB or no divider fitted

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

NimBLECharacteristic* buzzerChar;
NimBLECharacteristic* statusChar;
NimBLECharacteristic* playersChar;

GameState state = STATE_IDLE;
uint8_t   winner = 0;
uint16_t  roundNo = 0;
bool      bleConnected = false;
uint32_t  lastBroadcast = 0;
uint32_t  roundStartMs = 0;
uint32_t  buzzMs = 0;

// "ER" variant = the 0.42" panel's column offset
U8G2_SSD1306_72X40_ER_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
volatile bool oledDirty = true;
uint32_t lastOled = 0;
uint32_t lastPlayersCheck = 0;

struct PlayerInfo { uint32_t lastSeen; uint16_t battMv; };
PlayerInfo players[MAX_PLAYERS + 1];   // index 1..MAX_PLAYERS

// Everything (ESP-NOW receive, BLE writes) goes through one queue and is
// handled in loop(). Queue order = arrival order, so "first press wins"
// is decided in one place.
enum EvType : uint8_t { EV_PRESS, EV_HELLO, EV_START, EV_RESET };
struct Event { EvType type; uint8_t player; uint16_t round; uint16_t battMv; int8_t rssi; uint32_t t; };
QueueHandle_t events;

// ── ESP-NOW receive (runs in the WiFi task — keep it short) ───────
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(BuzzMsg)) return;
  const BuzzMsg* m = (const BuzzMsg*)data;
  if (m->magic != BUZZ_MAGIC) return;

  Event ev;
  ev.t = micros();
  ev.player = m->player;
  ev.round = m->round;
  ev.battMv = m->battMv;
  ev.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  if (m->type == MSG_PRESS)      ev.type = EV_PRESS;
  else if (m->type == MSG_HELLO) ev.type = EV_HELLO;
  else return;
  xQueueSend(events, &ev, 0);
}

// ── BLE callbacks ─────────────────────────────────────────────────
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    bleConnected = true;
    oledDirty = true;
    Serial.println("[BLE] browser connected");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    bleConnected = false;
    oledDirty = true;
    Serial.printf("[BLE] disconnected (reason %d), advertising again\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

class WriteCB : public NimBLECharacteristicCallbacks {
 public:
  explicit WriteCB(EvType t) : type(t) {}
  void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) override {
    Event ev = {type, 0, 0, 0, 0, micros()};
    xQueueSend(events, &ev, 0);
  }
 private:
  EvType type;
};

// ── Helpers ───────────────────────────────────────────────────────
void broadcastState() {
  BuzzMsg m = {BUZZ_MAGIC, MSG_STATE, winner, (uint8_t)state, 0, roundNo};
  esp_now_send(BROADCAST_ADDR, (const uint8_t*)&m, sizeof(m));
  lastBroadcast = millis();
}

void setState(GameState s, uint8_t w) {
  state = s;
  winner = w;
  oledDirty = true;

  statusChar->setValue((uint8_t)state);
  statusChar->notify();
  buzzerChar->setValue(winner);
  if (state == STATE_BUZZED) buzzerChar->notify();  // page only reacts to p > 0

  // Send the change a few times; ESP-NOW broadcast has no ACK.
  for (int i = 0; i < 3; i++) { broadcastState(); delay(2); }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(BUZZ_CHANNEL, WIFI_SECOND_CHAN_NONE);
  // Note: do NOT set WIFI_PS_NONE here — it isn't allowed while BLE is running.

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;          // 0 = current channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.printf("[ESPNOW] hub MAC %s, channel %d\n", WiFi.macAddress().c_str(), BUZZ_CHANNEL);
}

void setupBle() {
  NimBLEDevice::init("QuizBuzzer");
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  NimBLEService* svc = server->createService(SERVICE_UUID);
  buzzerChar = svc->createCharacteristic(BUZZER_STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusChar = svc->createCharacteristic(STATUS_UUID,       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  playersChar = svc->createCharacteristic(PLAYERS_UUID,     NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->createCharacteristic(RESET_UUID,       NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(new WriteCB(EV_RESET));
  svc->createCharacteristic(ROUND_START_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(new WriteCB(EV_START));
  buzzerChar->setValue((uint8_t)0);
  statusChar->setValue((uint8_t)STATE_IDLE);
  uint8_t noPlayers[1 + 2 * MAX_PLAYERS] = {0};
  playersChar->setValue(noPlayers, sizeof(noPlayers));
  svc->start();

  // Name only in the advert: name + a 128-bit UUID doesn't fit in 31 bytes.
  // The page filters on name and lists the service in optionalServices, so that's enough.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName("QuizBuzzer");
  adv->start();
  Serial.println("[BLE] advertising as QuizBuzzer");
}

void updateLed() {
  bool on;
  switch (state) {
    case STATE_ACTIVE: on = (millis() / 125) % 2; break;   // fast blink
    case STATE_BUZZED: on = true; break;                   // solid
    default:           on = bleConnected ? false : (millis() % 2000) < 50; // blip = waiting for browser
  }
  digitalWrite(STATUS_LED, on ? LOW : HIGH);
}

// ── OLED ──────────────────────────────────────────────────────────
// Top: browser link + round (or reaction time). Middle: READY / timer / winner.
// Bottom: one cell per player — filled = online, blinking = low battery.
bool playerOnline(int p) { return players[p].lastSeen && millis() - players[p].lastSeen < PLAYER_TIMEOUT_MS; }
bool playerLowBatt(int p) { return players[p].battMv > NO_BATT_MV && players[p].battMv < LOW_BATT_MV; }

void drawCentered(const char* s, int y) {
  oled.drawStr((oled.getDisplayWidth() - oled.getStrWidth(s)) / 2, y, s);
}

void drawPlayers() {
  oled.setFont(u8g2_font_5x7_tr);
  bool blink = (millis() / 500) % 2;
  for (int p = 1; p <= MAX_PLAYERS; p++) {
    int x = (p - 1) * 18 + 2;
    char d[2] = {char('0' + p), 0};
    if (playerOnline(p)) {
      if (playerLowBatt(p) && blink) oled.drawFrame(x, 31, 14, 9);
      else { oled.drawBox(x, 31, 14, 9); oled.setDrawColor(0); }
    }
    oled.drawStr(x + 5, 39, d);
    oled.setDrawColor(1);
  }
}

void updateOled() {
  // Redraw on change, plus periodically for the timer, blink and online status.
  // A full redraw takes ~10 ms of I2C; events keep queueing meanwhile, so it
  // can't change who wins.
  uint32_t interval = state == STATE_ACTIVE ? 100 : 500;
  if (!oledDirty && millis() - lastOled < interval) return;
  oledDirty = false;
  lastOled = millis();

  char buf[12];
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x7_tr);
  oled.drawStr(0, 6, bleConnected ? "Web ok" : "No web");
  if (state == STATE_BUZZED) snprintf(buf, sizeof(buf), "%.2fs", (buzzMs - roundStartMs) / 1000.0);
  else if (roundNo)          snprintf(buf, sizeof(buf), "R%u", roundNo);
  else                       buf[0] = 0;
  oled.drawStr(oled.getDisplayWidth() - oled.getStrWidth(buf), 6, buf);

  oled.setFont(u8g2_font_logisoso16_tr);
  switch (state) {
    case STATE_IDLE:   drawCentered("READY", 27); break;
    case STATE_ACTIVE: snprintf(buf, sizeof(buf), "%.1f", (millis() - roundStartMs) / 1000.0); drawCentered(buf, 27); break;
    case STATE_BUZZED: snprintf(buf, sizeof(buf), "P%u", winner); drawCentered(buf, 27); break;
  }

  drawPlayers();
  oled.sendBuffer();
}

// ── Player presence over BLE ──────────────────────────────────────
// PLAYERS_UUID value: byte 0 = online bitmask (bit 0 = player 1),
// then uint16 little-endian battery mV per player (0 = unknown / no battery).
// Checked twice a second; only notified when something changed.
void updatePlayersChar() {
  if (millis() - lastPlayersCheck < 500) return;
  lastPlayersCheck = millis();

  static uint8_t last[1 + 2 * MAX_PLAYERS];
  uint8_t buf[1 + 2 * MAX_PLAYERS] = {0};
  for (int p = 1; p <= MAX_PLAYERS; p++) {
    if (playerOnline(p)) buf[0] |= 1 << (p - 1);
    buf[1 + 2 * (p - 1)] = players[p].battMv & 0xFF;
    buf[2 + 2 * (p - 1)] = players[p].battMv >> 8;
  }
  if (memcmp(buf, last, sizeof(buf)) == 0) return;
  memcpy(last, buf, sizeof(buf));

  playersChar->setValue(buf, sizeof(buf));
  playersChar->notify();
}

// ── Setup / loop ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(STATUS_LED, OUTPUT);
  events = xQueueCreate(32, sizeof(Event));

  oled.begin();
  oled.setBusClock(400000);

  setupEspNow();
  setupBle();
  Serial.println("[HUB] ready — send 's' = start, 'r' = reset over serial to test without the browser");
}

void loop() {
  Event ev;
  while (xQueueReceive(events, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case EV_START:
        roundNo++;
        roundStartMs = millis();
        setState(STATE_ACTIVE, 0);
        Serial.printf("[HUB] round %u started\n", roundNo);
        break;

      case EV_RESET:
        setState(STATE_IDLE, 0);
        Serial.println("[HUB] reset");
        break;

      case EV_PRESS:
        if (ev.player >= 1 && ev.player <= MAX_PLAYERS) players[ev.player].lastSeen = millis();
        // Only the first press of the current round counts. Late presses,
        // retries and presses tagged with an old round are ignored.
        if (state == STATE_ACTIVE && ev.round == roundNo) {
          buzzMs = millis();
          setState(STATE_BUZZED, ev.player);
          Serial.printf("[HUB] >>> player %u buzzed first (rssi %d)\n", ev.player, ev.rssi);
        }
        break;

      case EV_HELLO:
        if (ev.player >= 1 && ev.player <= MAX_PLAYERS) {
          if (!playerOnline(ev.player)) oledDirty = true;
          players[ev.player].lastSeen = millis();
          players[ev.player].battMv = ev.battMv;
        }
        Serial.printf("[HUB] player %u alive, battery %.2f V, rssi %d dBm\n",
                      ev.player, ev.battMv / 1000.0, ev.rssi);
        break;
    }
  }

  // Serial test commands, so you can test the radio link before the browser.
  if (Serial.available()) {
    char c = Serial.read();
    Event e = {c == 's' ? EV_START : EV_RESET, 0, 0, 0, 0, micros()};
    if (c == 's' || c == 'r') xQueueSend(events, &e, 0);
  }

  if (millis() - lastBroadcast > STATE_BROADCAST_MS) broadcastState();
  updateLed();
  updateOled();
  updatePlayersChar();
  delay(1);
}
