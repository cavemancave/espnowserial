#pragma once

#include <Arduino.h>

#include "settings.h"

// Every ESP-NOW frame starts with one of these bytes so that control traffic
// and transparent user data can share the same radio link.
enum PktType : uint8_t {
  PKT_DATA     = 0x01,  // transparent payload (the actual serial bytes)
  PKT_PAIR_REQ = 0x02,  // broadcast: "is anybody out there looking for a peer?"
  PKT_PAIR_ACK = 0x03,  // unicast:   "yes - here is my MAC address"
  PKT_PING     = 0x04,  // keep-alive, refreshes the peer online state
  PKT_INFO     = 0x05,  // human readable text (AT+TEST)
};

namespace Link {

using LogFn   = void (*)(const char *message);
using DataFn  = void (*)(const uint8_t *data, size_t len);
using EventFn = void (*)(const char *event);  // "paired" | "unpaired" | "online" | "offline"

void setLogger(LogFn fn);
void setDataHandler(DataFn fn);   // transparent serial payload
void setInfoHandler(DataFn fn);   // human readable text sent by AT+TEST
void setEventHandler(EventFn fn);

bool begin();
void loop();

bool sendData(const uint8_t *data, size_t len);
bool sendInfo(const char *text);

bool           isPaired();
bool           isPeerOnline();
const uint8_t *peerMac();
const uint8_t *selfMac();
String         selfMacString();

void adoptPeer(const uint8_t *mac);
void forgetPeer();
bool setChannel(uint8_t channel);

uint32_t txPackets();
uint32_t rxPackets();
uint32_t txErrors();
uint32_t rxDropped();
uint32_t bytesTx();
uint32_t bytesRx();

}  // namespace Link
