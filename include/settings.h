#pragma once

#include <Arduino.h>

// Persistent configuration, stored in the ESP32 NVS flash partition.
struct DeviceSettings {
  uint8_t  peerMac[6];   // MAC of the paired peer
  bool     hasPeer;      // is peerMac valid?
  uint32_t baud;         // console / data baud rate
  uint8_t  channel;      // Wi-Fi channel shared by both ends
  bool     autoPair;     // pair automatically with the first peer found
  bool     verbose;      // print status messages while in data mode
};

// Global settings instance.
DeviceSettings &settings();

void settingsBegin();      // load from NVS (call once in setup)
void settingsSave();       // write the current values back to NVS
void settingsClearPeer();  // drop the stored peer

// "AA:BB:CC:DD:EE:FF" -> 6 bytes (separators are optional / ignored).
bool settingsParseMac(const String &text, uint8_t *outMac);
String settingsFormatMac(const uint8_t *mac);
