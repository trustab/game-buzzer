// buzzer_hub.ino — PoC hub: ESP-NOW from buttons, BLE to the existing web page.
// Board: ESP32-C3 SuperMini ("ESP32C3 Dev Module", USB CDC On Boot: Enabled)
// Library: NimBLE-Arduino 2.x
//
// BLE side is identical to the old UNO R4 sketch (name "QuizBuzzer", same UUIDs,
// same byte values), so the React page works unchanged.

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <NimBLEDevice.h>
#include "protocol.h"

// ── BLE UUIDs (same as the web page) ──────────────────────────────
#define SERVICE_UUID      "12345678-1234-1234-1234-123456789abc"
#define BUZZER_STATE_UUID "12345678-1234-1234-1234-123456789ab1"
#define RESET_UUID        "12345678-1234-1234-1234-123456789ab2"
#define ROUND_START_UUID  "12345678-1234-1234-1234-123456789ab3"
#define STATUS_UUID       "12345678-1234-1234-1234-123456789ab4"

#define STATUS_LED 8            // SuperMini onboard LED, active LOW
#define STATE_BROADCAST_MS 500  // heartbeat so buttons always know the state

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

NimBLECharacteristic* buzzerChar;
NimBLECharacteristic* statusChar;

GameState state = STATE_IDLE;
uint8_t   winner = 0;
uint16_t  roundNo = 0;
bool      bleConnected = false;
uint32_t  lastBroadcast = 0;

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
    Serial.println("[BLE] browser connected");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    bleConnected = false;
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
  esp_now_send(BROADCAST, (const uint8_t*)&m, sizeof(m));
  lastBroadcast = millis();
}

void setState(GameState s, uint8_t w) {
  state = s;
  winner = w;

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
  memcpy(peer.peer_addr, BROADCAST, 6);
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
  svc->createCharacteristic(RESET_UUID,       NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(new WriteCB(EV_RESET));
  svc->createCharacteristic(ROUND_START_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(new WriteCB(EV_START));
  buzzerChar->setValue((uint8_t)0);
  statusChar->setValue((uint8_t)STATE_IDLE);
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

// ── Setup / loop ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(STATUS_LED, OUTPUT);
  events = xQueueCreate(32, sizeof(Event));

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
        setState(STATE_ACTIVE, 0);
        Serial.printf("[HUB] round %u started\n", roundNo);
        break;

      case EV_RESET:
        setState(STATE_IDLE, 0);
        Serial.println("[HUB] reset");
        break;

      case EV_PRESS:
        // Only the first press of the current round counts. Late presses,
        // retries and presses tagged with an old round are ignored.
        if (state == STATE_ACTIVE && ev.round == roundNo) {
          setState(STATE_BUZZED, ev.player);
          Serial.printf("[HUB] >>> player %u buzzed first (rssi %d)\n", ev.player, ev.rssi);
        }
        break;

      case EV_HELLO:
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
  delay(1);
}
