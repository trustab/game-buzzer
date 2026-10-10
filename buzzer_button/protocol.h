// protocol.h — shared ESP-NOW message format.
// Copy this file into BOTH sketch folders (buzzer_hub/ and buzzer_button/).
#pragma once
#include <stdint.h>

#define BUZZ_MAGIC   0xB7   // ignore any ESP-NOW traffic that isn't ours
#define BUZZ_CHANNEL 1      // hub and buttons must use the same WiFi channel

enum MsgType : uint8_t {
  MSG_PRESS = 1,   // button -> hub : "I pressed"
  MSG_HELLO = 2,   // button -> hub : heartbeat with battery level
  MSG_STATE = 3,   // hub -> all    : current game state + winner
};

enum GameState : uint8_t {
  STATE_IDLE   = 0,   // matches the web page: 0=idle 1=active 2=buzzed
  STATE_ACTIVE = 1,
  STATE_BUZZED = 2,
};

struct __attribute__((packed)) BuzzMsg {
  uint8_t  magic;    // BUZZ_MAGIC
  uint8_t  type;     // MsgType
  uint8_t  player;   // sender's player ID (button) or winner ID (hub, 0 = none)
  uint8_t  state;    // GameState (hub -> buttons only)
  uint16_t battMv;   // battery voltage in mV (button -> hub only)
  uint16_t round;    // round counter (hub increments on every Start)
};
