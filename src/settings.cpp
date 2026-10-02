#include "settings.h"

#include <Preferences.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace {
constexpr const char *NS = "espnowser";

Preferences    gPrefs;
DeviceSettings gSettings;
}  // namespace

DeviceSettings &settings() { return gSettings; }

void settingsBegin() {
  memset(&gSettings, 0, sizeof(gSettings));
  gSettings.baud     = DEFAULT_BAUD;
  gSettings.channel  = DEFAULT_CHANNEL;
  gSettings.autoPair = DEFAULT_AUTOPAIR;
  gSettings.verbose  = DEFAULT_VERBOSE;

  gPrefs.begin(NS, false);

  gSettings.baud     = gPrefs.getUInt("baud", DEFAULT_BAUD);
  gSettings.channel  = (uint8_t)gPrefs.getUChar("chan", DEFAULT_CHANNEL);
  gSettings.autoPair = gPrefs.getBool("autopair", DEFAULT_AUTOPAIR);
  gSettings.verbose  = gPrefs.getBool("verbose", DEFAULT_VERBOSE);

  // A stored baud rate from an older build would otherwise hide a new default.
  if (gPrefs.getUInt("cfgver", 1) < SETTINGS_VERSION) {
    gSettings.baud = DEFAULT_BAUD;
    gPrefs.putUInt("cfgver", SETTINGS_VERSION);
    gPrefs.putUInt("baud", DEFAULT_BAUD);
  }

  gSettings.hasPeer = gPrefs.getBool("paired", false);
  if (gSettings.hasPeer) {
    if (gPrefs.getBytesLength("peer") == sizeof(gSettings.peerMac)) {
      gPrefs.getBytes("peer", gSettings.peerMac, sizeof(gSettings.peerMac));
    } else {
      gSettings.hasPeer = false;
    }
  }

  // Sanity checks against values that could have been written by an older build.
  if (gSettings.channel < 1 || gSettings.channel > 13) gSettings.channel = DEFAULT_CHANNEL;
  if (gSettings.baud < 300 || gSettings.baud > 2000000) gSettings.baud = DEFAULT_BAUD;
}

void settingsSave() {
  gPrefs.putBool("paired", gSettings.hasPeer);
  if (gSettings.hasPeer) gPrefs.putBytes("peer", gSettings.peerMac, sizeof(gSettings.peerMac));
  gPrefs.putUInt("baud", gSettings.baud);
  gPrefs.putUChar("chan", gSettings.channel);
  gPrefs.putBool("autopair", gSettings.autoPair);
  gPrefs.putBool("verbose", gSettings.verbose);
}

void settingsClearPeer() {
  memset(gSettings.peerMac, 0, sizeof(gSettings.peerMac));
  gSettings.hasPeer = false;
  gPrefs.putBool("paired", false);
  gPrefs.remove("peer");
}

bool settingsParseMac(const String &text, uint8_t *outMac) {
  String hex;
  hex.reserve(12);
  for (unsigned int i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (isxdigit((unsigned char)c)) hex += c;
  }
  if (hex.length() != 12) return false;

  for (int i = 0; i < 6; ++i) {
    outMac[i] = (uint8_t)strtol(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  }
  return true;
}

String settingsFormatMac(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  return String(buf);
}
