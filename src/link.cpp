#include "link.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

// arduino-esp32 v3 changed the ESP-NOW callback signatures.  Keep the default
// at v2 so the file also compiles against an older platform package.
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// ------------------------------------------------------------------ module state
namespace {
Link::LogFn   gLog   = nullptr;
Link::DataFn  gData  = nullptr;
Link::DataFn  gInfo  = nullptr;
Link::EventFn gEvent = nullptr;

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t  gSelfMac[6]     = {0};
bool     gStarted        = false;
uint32_t gLastRxMs       = 0;
uint32_t gLastPingMs     = 0;
uint32_t gLastBeaconMs   = 0;
uint32_t gTxPackets      = 0;
uint32_t gRxPackets      = 0;
uint32_t gTxErrors       = 0;
uint32_t gRxDropped      = 0;
uint32_t gBytesTx        = 0;
uint32_t gBytesRx        = 0;
bool     gWasOnline      = false;

// Pairing decisions are taken inside the radio callback but carried out later
// by the main loop: writing to flash and pushing a frame from within the Wi-Fi
// task is slow and can make it drop packets.
portMUX_TYPE gPairMux     = portMUX_INITIALIZER_UNLOCKED;
uint8_t      gPairMac[6]  = {0};
bool         gPairAdopt   = false;
bool         gPairAck     = false;

void logf(const char *fmt, ...) {
  if (gLog == nullptr) return;
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  gLog(buf);
}

void emit(const char *event) {
  if (gEvent != nullptr) gEvent(event);
}

// Called from the radio callback: remember what to do, do not act yet.
void requestPairing(const uint8_t *mac, bool adopt, bool acknowledge) {
  portENTER_CRITICAL(&gPairMux);
  memcpy(gPairMac, mac, 6);
  gPairAdopt = adopt;
  gPairAck   = acknowledge;
  portEXIT_CRITICAL(&gPairMux);
}

bool addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
  peer.channel = settings().channel;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

// Prepend the packet type and hand the frame to the radio.
bool sendPkt(const uint8_t *mac, uint8_t type, const uint8_t *payload, size_t len) {
  uint8_t frame[ESPNOW_MAX_PAYLOAD];
  if (len + 1 > sizeof(frame)) return false;
  if (!addPeer(mac)) {
    gTxErrors++;
    return false;
  }

  frame[0] = type;
  if (len > 0 && payload != nullptr) memcpy(frame + 1, payload, len);

  // The radio TX queue has a limited depth; on a burst it can be busy for a
  // moment, so give it a few short chances before giving up on the frame.
  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 5; ++attempt) {
    err = esp_now_send(mac, frame, len + 1);
    if (err == ESP_OK) break;
    delay(1);
  }
  if (err != ESP_OK) {
    gTxErrors++;
    return false;
  }

  gTxPackets++;
  gBytesTx += len;
  return true;
}

void handleRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 1) return;

  const uint8_t  type = data[0];
  const uint8_t *payload = data + 1;
  const size_t   plen = (size_t)(len - 1);

  DeviceSettings &st = settings();
  const bool fromPeer = st.hasPeer && memcmp(mac, st.peerMac, sizeof(st.peerMac)) == 0;

  switch (type) {
    case PKT_PAIR_REQ:
      if (st.hasPeer) {
        // Our own partner rebooted with a cleared configuration: teach it again.
        if (fromPeer) requestPairing(mac, false, true);
        return;
      }
      if (!st.autoPair) return;
      requestPairing(mac, true, true);
      return;

    case PKT_PAIR_ACK:
      if (!fromPeer) {
        if (st.hasPeer || !st.autoPair) {
          gRxDropped++;
          return;
        }
        // The sender already knows us, so no acknowledgement is needed back.
        requestPairing(mac, true, false);
        return;
      }
      gLastRxMs = millis();
      return;

    default:
      break;
  }

  // Everything below is only accepted from the paired peer.
  if (!fromPeer) {
    gRxDropped++;
    return;
  }

  gLastRxMs = millis();
  gRxPackets++;

  switch (type) {
    case PKT_DATA:
      gBytesRx += plen;
      if (gData != nullptr && plen > 0) gData(payload, plen);
      break;

    case PKT_INFO:
      if (gInfo != nullptr && plen > 0) gInfo(payload, plen);
      break;

    case PKT_PING:
    default:
      break;  // nothing to do - the timestamp above is all we need
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecvCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  handleRecv(info->src_addr, data, len);
}
#else
void onRecvCb(const uint8_t *mac, const uint8_t *data, int len) { handleRecv(mac, data, len); }
#endif

}  // namespace

// ------------------------------------------------------------------ public API
namespace Link {

void setLogger(LogFn fn) { gLog = fn; }
void setDataHandler(DataFn fn) { gData = fn; }
void setInfoHandler(DataFn fn) { gInfo = fn; }
void setEventHandler(EventFn fn) { gEvent = fn; }

const uint8_t *selfMac() { return gSelfMac; }
String selfMacString() { return settingsFormatMac(gSelfMac); }

bool begin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);  // keeps ESP-NOW latency low
  delay(20);
  WiFi.macAddress(gSelfMac);

  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onRecvCb);

  // Both ends must sit on the same channel; a fixed one avoids surprises.
  esp_wifi_set_channel(settings().channel, WIFI_SECOND_CHAN_NONE);

  addPeer(BROADCAST_MAC);
  if (settings().hasPeer) addPeer(settings().peerMac);

  // Start out "offline": a paired peer that is switched off must not look
  // online during the first PEER_TIMEOUT_MS after our own boot.  The unsigned
  // subtraction in isPeerOnline() makes this wrap safely.
  gLastRxMs = millis() - PEER_TIMEOUT_MS;
  gStarted  = true;
  return true;
}

void loop() {
  const uint32_t  now = millis();
  DeviceSettings &st = settings();

  // Carry out a pairing decided by the radio callback.
  portENTER_CRITICAL(&gPairMux);
  const bool adopt = gPairAdopt;
  const bool ack   = gPairAck;
  uint8_t    mac[6];
  memcpy(mac, gPairMac, sizeof(mac));
  gPairAdopt = false;
  gPairAck   = false;
  portEXIT_CRITICAL(&gPairMux);

  if (ack) sendPkt(mac, PKT_PAIR_ACK, gSelfMac, sizeof(gSelfMac));
  if (adopt) {
    const bool isNew = !st.hasPeer || memcmp(st.peerMac, mac, sizeof(mac)) != 0;
    Link::adoptPeer(mac);
    if (isNew) logf("paired with %s", settingsFormatMac(mac).c_str());
  }

  const bool online = isPeerOnline();
  if (online != gWasOnline) {
    gWasOnline = online;
    emit(online ? "online" : "offline");
  }

  if (st.hasPeer) {
    if (now - gLastPingMs >= HEARTBEAT_INTERVAL_MS) {
      gLastPingMs = now;
      sendPkt(st.peerMac, PKT_PING, nullptr, 0);
    }
  } else if (st.autoPair) {
    if (now - gLastBeaconMs >= AUTOPAIR_INTERVAL_MS) {
      gLastBeaconMs = now;
      sendPkt(BROADCAST_MAC, PKT_PAIR_REQ, gSelfMac, sizeof(gSelfMac));
    }
  }
}

bool sendData(const uint8_t *data, size_t len) {
  DeviceSettings &st = settings();
  if (!st.hasPeer) return false;

  bool   ok  = true;
  size_t off = 0;
  while (off < len) {
    size_t chunk = len - off;
    if (chunk > ESPNOW_CHUNK) chunk = ESPNOW_CHUNK;
    if (!sendPkt(st.peerMac, PKT_DATA, data + off, chunk)) ok = false;
    off += chunk;
  }
  return ok;
}

bool sendInfo(const char *text) {
  DeviceSettings &st = settings();
  if (!st.hasPeer) return false;
  return sendPkt(st.peerMac, PKT_INFO, (const uint8_t *)text, strlen(text));
}

bool isPaired() { return settings().hasPeer; }

bool isPeerOnline() {
  return settings().hasPeer && (uint32_t)(millis() - gLastRxMs) < PEER_TIMEOUT_MS;
}

const uint8_t *peerMac() { return settings().peerMac; }

void adoptPeer(const uint8_t *mac) {
  DeviceSettings &st = settings();
  const bool changed = !st.hasPeer || memcmp(st.peerMac, mac, sizeof(st.peerMac)) != 0;

  memcpy(st.peerMac, mac, sizeof(st.peerMac));
  st.hasPeer = true;
  settingsSave();
  addPeer(mac);

  gLastRxMs = millis();
  if (changed) emit("paired");
}

void forgetPeer() {
  DeviceSettings &st = settings();
  if (st.hasPeer) esp_now_del_peer(st.peerMac);

  settingsClearPeer();
  gWasOnline = false;
  emit("unpaired");
}

bool setChannel(uint8_t channel) {
  if (channel < 1 || channel > 13) return false;

  DeviceSettings &st = settings();
  st.channel = channel;
  settingsSave();
  if (!gStarted) return true;

  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  // Peers remember the channel they were registered with, so re-register them.
  if (st.hasPeer) {
    esp_now_del_peer(st.peerMac);
    addPeer(st.peerMac);
  }
  esp_now_del_peer(BROADCAST_MAC);
  addPeer(BROADCAST_MAC);
  return true;
}

uint32_t txPackets() { return gTxPackets; }
uint32_t rxPackets() { return gRxPackets; }
uint32_t txErrors() { return gTxErrors; }
uint32_t rxDropped() { return gRxDropped; }
uint32_t bytesTx() { return gBytesTx; }
uint32_t bytesRx() { return gBytesRx; }

}  // namespace Link
