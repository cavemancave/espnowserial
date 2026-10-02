// ===========================================================================
//  ESP-NOW wireless serial
//  Target: 2 x ESP32-C3 Pro Mini (HW-953AB), onboard LED on GPIO8.
//  No expansion board and no RS-485: the USB / TTL console is bridged over air.
//
//      PC  <--USB-->  board A  <--ESP-NOW-->  board B  <--USB-->  PC
//
//  Transparent by default, "+++" switches to an AT command console.
// ===========================================================================

#include <Arduino.h>
#include <driver/gpio.h>
#include <string.h>

#include "config.h"
#include "link.h"
#include "settings.h"

// ===========================================================================
//  Console - the USB port and UART0 are mirrored so one firmware image works
//  on every ESP32-C3 Pro Mini USB revision (see config.h).
// ===========================================================================
static Stream *gPorts[3];
static uint8_t gPortCount = 0;

static bool   gConfigMode = false;  // false = transparent data, true = AT console
static String gLine;                // AT command being typed
static bool   gEcho = true;

static uint32_t gConsoleDropped = 0;

// HWCDC::write() gives up and reports a SHORT count when its TX ring stays
// full (or when the host is not draining), so a single write() must never be
// trusted to have sent everything - feed it the remainder.
static void consoleWriteFull(Stream &port, const uint8_t *data, size_t len) {
  size_t off    = 0;
  int    stalls = 0;
  while (off < len && stalls < 4) {
    const size_t written = port.write(data + off, len - off);
    if (written == 0) {
      ++stalls;
      delay(1);
      continue;
    }
    stalls = 0;
    off += written;
  }
  gConsoleDropped += (uint32_t)(len - off);
}

// The UART0 tap is a convenience mirror; it must never slow the bridge down,
// so it only takes what fits in its transmit buffer right now.  A device that
// really is attached should use AT+BAUD to keep up.
static void consoleWriteMirror(Stream &port, const uint8_t *data, size_t len) {
  const int room = port.availableForWrite();
  if (room <= 0) {
    gConsoleDropped += (uint32_t)len;
    return;
  }
  size_t take = (size_t)room < len ? (size_t)room : len;
  take = port.write(data, take);  // may still come up short
  gConsoleDropped += (uint32_t)(len - take);
}

static void consoleOut(const uint8_t *data, size_t len) {
  if (gPortCount > 0) consoleWriteFull(*gPorts[0], data, len);  // USB, lossless
  for (uint8_t i = 1; i < gPortCount; ++i) consoleWriteMirror(*gPorts[i], data, len);
}

static void consolePrint(const char *text) { consoleOut((const uint8_t *)text, strlen(text)); }

static void consolePrintln(const char *text) {
  consolePrint(text);
  consolePrint("\r\n");
}

static void consolePrintln(const String &text) { consolePrintln(text.c_str()); }

// ===========================================================================
//  Onboard LED (active low)
// ===========================================================================
static bool     gLedOn      = false;
static uint32_t gLedNextMs  = 0;
static volatile uint32_t gActivityMs = 0;  // also written from the Wi-Fi task

static inline void ledWrite(bool on) {
#if LED_ACTIVE_LOW
  digitalWrite(PIN_LED, on ? LOW : HIGH);
#else
  digitalWrite(PIN_LED, on ? HIGH : LOW);
#endif
}

static void ledUpdate() {
  const uint32_t now = millis();
  bool           state;

  if (!Link::isPaired()) {
    if (now >= gLedNextMs) {
      gLedNextMs = now + LED_BLINK_SLOW_MS;
      gLedOn = !gLedOn;
    }
    state = gLedOn;                                 // slow blink: looking for a peer
  } else if (!Link::isPeerOnline()) {
    if (now >= gLedNextMs) {
      gLedNextMs = now + LED_BLINK_FAST_MS;
      gLedOn = !gLedOn;
    }
    state = gLedOn;                                 // fast blink: peer stopped answering
  } else {
    gLedOn = true;
    state  = true;                                  // solid on: link is up
  }

  if (now < gActivityMs) state = !state;            // short flicker on traffic
  ledWrite(state);
}

// ===========================================================================
//  Outgoing path: console bytes -> coalescing buffer -> ESP-NOW
// ===========================================================================
static uint8_t  gTxBuf[ESPNOW_CHUNK];
static size_t   gTxLen     = 0;
static uint32_t gTxFlushMs = 0;
static uint32_t gLastDataMs   = 0;
static bool     gStreamActive = false;

// Escape state.
//
// NEVER print the literal three-plus sequence from the firmware.  With a
// loopback (TX jumpered to RX, or any echoing device on the port) that text
// would come straight back in, be taken for the escape and push the firmware
// back into config mode - whose reply would do the same again, forever.
static uint8_t  gPlusRun     = 0;   // consecutive '+' held for the escape
static uint32_t gPlusStartMs = 0;
static uint8_t  gSwallowEol  = 0;   // CR/LF left over from the AT line, dropped

static void warnUnpaired() {
  if (!settings().verbose) return;
  static uint32_t last = 0;
  const uint32_t  now  = millis();
  if (now - last < 1000) return;
  last = now;
  consolePrint("\r\n[!] no peer paired - data dropped (send three '+' to configure)\r\n");
}

static void txFlush() {
  if (gTxLen == 0) return;

  if (Link::isPaired()) {
    Link::sendData(gTxBuf, gTxLen);
    gActivityMs = millis() + LED_ACTIVITY_MS;
  } else {
    warnUnpaired();
  }
  gTxLen = 0;
}

static void txPush(uint8_t byte) {
  if (gTxLen >= sizeof(gTxBuf)) txFlush();
  gTxBuf[gTxLen++] = byte;
  gTxFlushMs       = millis();
  gLastDataMs      = gTxFlushMs;
  gStreamActive    = true;
}

static void releaseHeldPlus() {
  while (gPlusRun > 0) {
    txPush('+');
    --gPlusRun;
  }
}

// ===========================================================================
//  Incoming path: ESP-NOW -> lock free ring buffer -> console
//
//  The radio callback runs inside the Wi-Fi task, so it must never block on a
//  (potentially slow) USB write - that would stall the Wi-Fi stack and drop
//  packets.  It only feeds a ring buffer which the main loop drains.
// ===========================================================================
static constexpr size_t RX_RING_SIZE = 8192;

static uint8_t  gRxRing[RX_RING_SIZE];
static uint32_t gRxHead = 0;  // producer index, radio callback only
static uint32_t gRxTail = 0;  // consumer index, main loop only
static uint32_t gRxOverflow = 0;

static void ringPush(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint32_t next = (gRxHead + 1) % RX_RING_SIZE;
    if (next == gRxTail) {
      ++gRxOverflow;  // buffer full, drop the rest of this frame
      return;
    }
    gRxRing[gRxHead] = data[i];
    gRxHead = next;
  }
}

static void ringPushStr(const char *text) { ringPush((const uint8_t *)text, strlen(text)); }

static void drainRxRing() {
  while (gRxTail != gRxHead) {
    uint8_t chunk[128];
    size_t  n = 0;
    while (gRxTail != gRxHead && n < sizeof(chunk)) {
      chunk[n++] = gRxRing[gRxTail];
      gRxTail = (gRxTail + 1) % RX_RING_SIZE;
    }
    consoleOut(chunk, n);
    gActivityMs = millis() + LED_ACTIVITY_MS;
  }
}

static void linkData(const uint8_t *data, size_t len) { ringPush(data, len); }

static void linkInfo(const uint8_t *data, size_t len) {
  ringPushStr("\r\n[peer] ");
  ringPush(data, len);
  ringPushStr("\r\n");
}

static void linkLog(const char *message) {
  if (settings().verbose || gConfigMode) consolePrintln(String("[link] ") + message);
}

static void linkEvent(const char *event) {
  if (!settings().verbose && !gConfigMode) return;
  if (strcmp(event, "online") == 0) {
    consolePrintln("[link] peer is online");
  } else if (strcmp(event, "offline") == 0) {
    consolePrintln("[link] peer is offline");
  }
}

// ===========================================================================
//  Mode handling
// ===========================================================================
static void processAt(const String &command);  // defined below

static void configEnter() {
  gConfigMode   = true;
  gTxLen        = 0;
  gPlusRun      = 0;
  gSwallowEol   = 0;
  gStreamActive = false;
  gLine         = "";
  consolePrint("\r\n");
  consolePrintln("=== " FW_NAME " " FW_VERSION " - ESP32-C3 Pro Mini ===");
  consolePrintln(String("MAC: ") + Link::selfMacString());
  consolePrintln(String("peer: ") +
                 (Link::isPaired() ? settingsFormatMac(Link::peerMac()) : String("not paired")));
  consolePrintln("[AT] configuration mode - AT+HELP for commands, ATO to resume data");
}

static void handleDataByte(uint8_t byte) {
  const uint32_t now = millis();

  // The terminator of the AT line that took us out of config mode is not data.
  if (gSwallowEol > 0 && (byte == '\r' || byte == '\n')) {
    --gSwallowEol;
    return;
  }
  gSwallowEol = 0;

  if (byte == '+') {
    // A "+" inside a data burst is ordinary data, not the start of an escape.
    if (gStreamActive && (now - gLastDataMs) < ESCAPE_GUARD_MS) {
      releaseHeldPlus();
      txPush(byte);
      return;
    }
    if (gPlusRun == 0) gPlusStartMs = now;
    if (++gPlusRun >= 3) {
      gPlusRun = 0;
      configEnter();
    }
    return;
  }

  releaseHeldPlus();  // the held "+" characters were data after all
  txPush(byte);
}

static void handleConfigByte(uint8_t byte) {
  if (byte == '\r' || byte == '\n') {
    if (gLine.length() > 0) {
      consolePrintln("");
      processAt(gLine);
      gLine = "";
    }
    return;
  }

  if (byte == 0x08 || byte == 0x7F) {  // backspace / delete
    if (gLine.length() > 0) {
      gLine.remove(gLine.length() - 1);
      if (gEcho) consolePrint("\b \b");
    }
    return;
  }

  if (byte < 0x20 || byte > 0x7E) return;  // printable ASCII only
  if (gLine.length() >= 128) return;

  gLine += (char)byte;
  if (gEcho) consoleOut(&byte, 1);
}

static bool pumpPort(Stream &port) {
  bool   got    = false;
  size_t budget = CONSOLE_RX_BUDGET;

  // Bounded on purpose: a port that refills as fast as we drain it (e.g. TX
  // jumpered to RX) must not be able to keep us in here forever, otherwise
  // loop() never returns and the other ports - USB included - starve.
  while (budget > 0 && port.available() > 0) {
    const int value = port.read();
    if (value < 0) break;
    got = true;
    --budget;
    if (gConfigMode) {
      handleConfigByte((uint8_t)value);
    } else {
      handleDataByte((uint8_t)value);
    }
  }
  // NOTE: nothing is flushed here on purpose.  Sending one radio frame per
  // incoming byte would swamp ESP-NOW; a short coalescing window (see loop())
  // groups the bytes into full sized frames instead.
  return got;
}

// ===========================================================================
//  AT command console
// ===========================================================================
static void atHelp() {
  consolePrintln("");
  consolePrintln("=== " FW_NAME " " FW_VERSION " ===");
  consolePrintln("AT+HELP              show this help");
  consolePrintln("AT+MAC               print this device MAC address");
  consolePrintln("AT+FRIEND            print the paired peer MAC address");
  consolePrintln("AT+PAIR=<MAC>        pair with a peer, e.g. AT+PAIR=AA:BB:CC:DD:EE:FF");
  consolePrintln("AT+UNPAIR            forget the paired peer");
  consolePrintln("AT+CHANNEL=<1-13>    radio channel, both ends must match (default 1)");
  consolePrintln("AT+BAUD=<rate>       console baud rate, default 115200");
  consolePrintln("AT+AUTOPAIR=<0|1>    pair automatically with the first peer found");
  consolePrintln("AT+VERBOSE=<0|1>     print status messages while in data mode");
  consolePrintln("AT+TEST              send a test packet to the peer");
  consolePrintln("AT+SYSINFO           system information and counters");
  consolePrintln("AT+SAVE              store the current settings in flash");
  consolePrintln("AT+RESTART           reboot the board");
  consolePrintln("ATO                  leave configuration mode");
  consolePrintln("");
  consolePrintln("In data mode send three '+' (1 s of silence first) to come back here.");
}

static String portSummary() {
  String s = "USB";
#if ENABLE_UART0_BRIDGE
  s += " + UART0(RX" + String(PIN_UART0_RX) + "/TX" + String(PIN_UART0_TX) + ")";
#endif
#if ENABLE_UART1
  s += " + UART1(RX" + String(PIN_UART1_RX) + "/TX" + String(PIN_UART1_TX) + ")";
#endif
  return s;
}

static void atSysInfo() {
  const char *state = !Link::isPaired() ? "not paired"
                      : Link::isPeerOnline() ? "paired / online"
                                             : "paired / offline";
  consolePrintln("");
  consolePrintln("=== system info ===");
  consolePrintln(String("firmware      : ") + FW_NAME + " " + FW_VERSION);
  consolePrintln(String("mac           : ") + Link::selfMacString());
  consolePrintln(String("peer mac      : ") +
                 (Link::isPaired() ? settingsFormatMac(Link::peerMac()) : String("not paired")));
  consolePrintln(String("link state    : ") + state);
  consolePrintln(String("radio channel : ") + String(settings().channel));
  consolePrintln(String("console baud  : ") + String(settings().baud));
  consolePrintln(String("ports         : ") + portSummary());
  consolePrintln(String("auto pair     : ") + (settings().autoPair ? "on" : "off"));
  consolePrintln(String("verbose       : ") + (settings().verbose ? "on" : "off"));
  consolePrintln(String("tx packets    : ") + String(Link::txPackets()) + " (" +
                 String(Link::bytesTx()) + " bytes)");
  consolePrintln(String("rx packets    : ") + String(Link::rxPackets()) + " (" +
                 String(Link::bytesRx()) + " bytes)");
  consolePrintln(String("tx errors     : ") + String(Link::txErrors()));
  consolePrintln(String("rx dropped    : ") + String(Link::rxDropped()));
  consolePrintln(String("rx overflow   : ") + String(gRxOverflow));
  consolePrintln(String("console drop  : ") + String(gConsoleDropped));
  consolePrintln(String("uptime        : ") + String(millis() / 1000) + " s");
  consolePrintln(String("free heap     : ") + String(ESP.getFreeHeap()) + " bytes");
  consolePrintln("");
}

static bool parseOnOff(const String &value, bool &out) {
  String v = value;
  v.trim();
  if (v == "1" || v.equalsIgnoreCase("on") || v.equalsIgnoreCase("true")) {
    out = true;
    return true;
  }
  if (v == "0" || v.equalsIgnoreCase("off") || v.equalsIgnoreCase("false")) {
    out = false;
    return true;
  }
  return false;
}

static void processAt(const String &command) {
  String cmd = command;
  cmd.trim();
  if (cmd.length() == 0) return;
  cmd.toUpperCase();

  if (cmd == "AT" || cmd == "AT+HELP") {
    atHelp();
    return;
  }

  if (cmd == "ATO") {
    gConfigMode   = false;
    gStreamActive = false;
    gPlusRun      = 0;
    gSwallowEol   = 2;   // eat the trailing CR/LF of this very command
    consolePrintln("[AT] data mode - bytes go over the air; send three '+' to come back");
    return;
  }

  if (cmd == "AT+MAC") {
    consolePrintln(Link::selfMacString());
    return;
  }

  if (cmd == "AT+FRIEND") {
    if (Link::isPaired()) {
      consolePrintln(settingsFormatMac(Link::peerMac()));
    } else {
      consolePrintln("ERROR: not paired");
    }
    return;
  }

  if (cmd.startsWith("AT+PAIR=")) {
    uint8_t mac[6] = {0};
    if (!settingsParseMac(cmd.substring(8), mac)) {
      consolePrintln("ERROR: invalid MAC, use AT+PAIR=AA:BB:CC:DD:EE:FF");
      return;
    }
    Link::adoptPeer(mac);
    consolePrintln(String("[AT] paired with ") + settingsFormatMac(mac));
    consolePrintln("[AT] on the other board run: AT+PAIR=" + Link::selfMacString());
    return;
  }

  if (cmd == "AT+UNPAIR" || cmd == "AT+CLEAR") {
    Link::forgetPeer();
    consolePrintln("[AT] peer forgotten");
    return;
  }

  if (cmd.startsWith("AT+CHANNEL=")) {
    const long channel = cmd.substring(11).toInt();
    if (!Link::setChannel((uint8_t)channel)) {
      consolePrintln("ERROR: channel must be 1..13");
    } else {
      consolePrintln(String("[AT] channel set to ") + String(channel) +
                     " - do the same on the other board");
    }
    return;
  }

  if (cmd.startsWith("AT+BAUD=")) {
    const long baud = cmd.substring(8).toInt();
    if (baud < 300 || baud > 2000000) {
      consolePrintln("ERROR: baud rate out of range");
      return;
    }
    settings().baud = (uint32_t)baud;
    settingsSave();
#if ENABLE_UART0_BRIDGE
    Serial0.updateBaudRate((unsigned long)baud);
#endif
#if ENABLE_UART1
    Serial1.updateBaudRate((unsigned long)baud);
#endif
    consolePrintln(String("[AT] baud rate set to ") + String(baud) +
                   " - set your terminal accordingly");
    return;
  }

  if (cmd.startsWith("AT+AUTOPAIR=")) {
    bool on = false;
    if (!parseOnOff(cmd.substring(12), on)) {
      consolePrintln("ERROR: use AT+AUTOPAIR=0 or AT+AUTOPAIR=1");
      return;
    }
    settings().autoPair = on;
    settingsSave();
    consolePrintln(String("[AT] auto pair ") + (on ? "enabled" : "disabled"));
    return;
  }

  if (cmd.startsWith("AT+VERBOSE=")) {
    bool on = false;
    if (!parseOnOff(cmd.substring(11), on)) {
      consolePrintln("ERROR: use AT+VERBOSE=0 or AT+VERBOSE=1");
      return;
    }
    settings().verbose = on;
    settingsSave();
    consolePrintln(String("[AT] verbose ") + (on ? "enabled" : "disabled"));
    return;
  }

  if (cmd == "AT+TEST") {
    if (!Link::isPaired()) {
      consolePrintln("ERROR: not paired");
      return;
    }
    const String text = String("TEST from ") + Link::selfMacString();
    consolePrintln(Link::sendInfo(text.c_str()) ? "[AT] test packet sent" : "ERROR: send failed");
    return;
  }

  if (cmd == "AT+SYSINFO") {
    atSysInfo();
    return;
  }

  if (cmd == "AT+SAVE") {
    settingsSave();
    consolePrintln("[AT] settings saved");
    return;
  }

  if (cmd == "AT+RESTART" || cmd == "AT+RESET") {
    consolePrintln("[AT] restarting");
    delay(100);
    ESP.restart();
  }

  consolePrintln("ERROR: unknown command - AT+HELP lists them all");
}

// ===========================================================================
//  setup / loop
// ===========================================================================
void setup() {
  pinMode(PIN_LED, OUTPUT);
  ledWrite(false);

  settingsBegin();

  // Must be done before begin(): the core default is only 256 bytes.
  Serial.setRxBufferSize(CONSOLE_RX_BUFFER);
  Serial.setTxBufferSize(CONSOLE_TX_BUFFER);
  Serial.begin(settings().baud);
#if ENABLE_UART0_BRIDGE
  Serial0.setRxBufferSize(CONSOLE_RX_BUFFER);
  Serial0.setTxBufferSize(CONSOLE_TX_BUFFER);
  Serial0.begin(settings().baud, SERIAL_8N1, PIN_UART0_RX, PIN_UART0_TX);
  // Keep a not-connected RX line idle instead of picking up noise, which would
  // otherwise be forwarded over the air as random bytes.
  gpio_set_pull_mode((gpio_num_t)PIN_UART0_RX, GPIO_PULLUP_ONLY);
#endif
#if ENABLE_UART1
  Serial1.setRxBufferSize(CONSOLE_RX_BUFFER);
  Serial1.setTxBufferSize(CONSOLE_TX_BUFFER);
  Serial1.begin(settings().baud, SERIAL_8N1, PIN_UART1_RX, PIN_UART1_TX);
  gpio_set_pull_mode((gpio_num_t)PIN_UART1_RX, GPIO_PULLUP_ONLY);
#endif

  gPorts[gPortCount++] = &Serial;          // USB CDC, lossless
#if ENABLE_UART0_BRIDGE
  gPorts[gPortCount++] = &Serial0;         // UART0, best-effort tap
#endif
#if ENABLE_UART1
  gPorts[gPortCount++] = &Serial1;         // UART1, best-effort tap
#endif

  Link::setLogger(linkLog);
  Link::setDataHandler(linkData);
  Link::setInfoHandler(linkInfo);
  Link::setEventHandler(linkEvent);

  const bool espnowOk = Link::begin();

  consolePrintln("");
  consolePrintln("=== " FW_NAME " " FW_VERSION " - ESP32-C3 Pro Mini ===");
  consolePrintln(String("MAC: ") + Link::selfMacString());

  if (!espnowOk) {
    consolePrintln("[!] ESP-NOW init failed - the radio will not work");
  } else if (Link::isPaired()) {
    consolePrintln(String("[link] restored peer ") + settingsFormatMac(Link::peerMac()));
  } else if (settings().autoPair) {
    consolePrintln("[link] looking for a peer ...");
  } else {
    consolePrintln("[link] auto pair is off - use AT+PAIR=<MAC>");
  }

  consolePrintln("[link] data mode: every byte goes over the air; three '+' opens the AT console");
  ledUpdate();
}

void loop() {
  bool active = false;

  for (uint8_t i = 0; i < gPortCount; ++i) {
    if (pumpPort(*gPorts[i])) active = true;
  }

  // Push whatever the radio received to the console.
  drainRxRing();

  // A held "+" that was not followed by two more is plain data after all.
  if (gPlusRun > 0 && (millis() - gPlusStartMs) > ESCAPE_GUARD_MS) releaseHeldPlus();

  // Ship a partially filled radio frame after a short coalescing window.
  if (gTxLen > 0 && !gConfigMode && (millis() - gTxFlushMs) >= TX_COALESCE_MS) txFlush();

  Link::loop();
  ledUpdate();

  if (!active) delay(1);
}