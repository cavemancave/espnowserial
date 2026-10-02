#pragma once

// ===========================================================================
//  ESP-NOW wireless serial  -  ESP32-C3 Pro Mini (HW-953AB)
//  No expansion board / no RS-485 and no extra parts required.
// ===========================================================================

#define FW_NAME     "espnow-serial"
#define FW_VERSION  "1.0.0"

// --------------------------------------------------------------- onboard LED
// The board LED is wired from 3V3 through a resistor to GPIO8  ->  LOW = lit.
// (GPIO8 is a boot strap pin as well; the LED pull-up keeps it happy at boot.)
#define PIN_LED            8
#define LED_ACTIVE_LOW     1

#define LED_BLINK_SLOW_MS  500   // waiting to be paired
#define LED_BLINK_FAST_MS  120   // paired, but the peer stopped answering
#define LED_ACTIVITY_MS     25   // short flicker while data is moving

// ------------------------------------------------------------- console ports
// The console (AT commands + transparent data) is mirrored on two ports so the
// same binary works on every ESP32-C3 Pro Mini revision:
//
//   * Serial  -> the ESP32-C3 native USB Serial/JTAG (USB-C connector)
//   * Serial0 -> UART0, on whatever pins PIN_UART0_RX / PIN_UART0_TX name
//
// Boards whose USB-C is driven by an external CH340 / CP2102 bridge wire that
// bridge to UART0 (GPIO20/21), so the very same code path still reaches the PC.
// On boards with native USB those header pins simply become a bonus TTL port.
// Set to 0 if you only ever want the USB port.
//
// UART0 is NOT tied to GPIO20/21: the ESP32-C3 routes UART signals through the
// GPIO matrix, so any free GPIO works - just change the two defines below.
//
//   usable on this board (broken out on the header): 0 1 3 4 5 6 7 10
//   usable but avoid : 2, 8, 9 are boot strapping pins (8 also drives the
//                      onboard LED, 9 is the BOOT button)
//   NOT usable       : 11..17 are wired to the in-package SPI flash
//                      18/19 are the native USB D-/D+ pins
//
// Why this is OFF here: on the HW-953AB the right-hand header ends with
// GPIO20/GPIO21, which sit right next to the PCB antenna.  With the bridge
// disabled nothing in the application ever drives those two pins.  Only the
// ROM bootloader still emits its 115200 startup log on GPIO21 for a few
// hundred ms after each reset - that is hardware default and cannot be
// disabled from here.  Set to 1 to get the extra TTL port back.
#define ENABLE_UART0_BRIDGE 0
#define PIN_UART0_RX        20
#define PIN_UART0_TX        21

#if ENABLE_UART0_BRIDGE
#  if PIN_UART0_RX < 0 || PIN_UART0_RX > 21 || PIN_UART0_TX < 0 || PIN_UART0_TX > 21
#    error "PIN_UART0_RX / PIN_UART0_TX must be a valid ESP32-C3 GPIO (0..21)"
#  endif
#  if PIN_UART0_RX == PIN_UART0_TX
#    error "PIN_UART0_RX and PIN_UART0_TX must be two different pins"
#  endif
#  if (PIN_UART0_RX >= 11 && PIN_UART0_RX <= 17) || (PIN_UART0_TX >= 11 && PIN_UART0_TX <= 17)
#    error "GPIO11..GPIO17 are wired to the in-package SPI flash and cannot be used"
#  endif
#  if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && \
      (PIN_UART0_RX == 18 || PIN_UART0_RX == 19 || PIN_UART0_TX == 18 || PIN_UART0_TX == 19)
#    error "GPIO18/19 are the native USB pins which this build is using - pick another pin"
#  endif
#endif

// ---------------------------------------------- optional third port: UART1
// UART1 ("Serial1") can be bridged exactly like the other two, giving a second
// TTL data port.  The C3 only has two UART controllers (UART0 + UART1), so this
// is the last one available.
//
// UART1 has NO default pins - unlike UART0 there is no hardware wiring behind
// it, so the two defines below are the only thing that decides where it shows
// up.  Set ENABLE_UART1 to 0 to leave the pins free for something else.
#define ENABLE_UART1  1
#define PIN_UART1_RX  4     // board silkscreen A4 / SCK
#define PIN_UART1_TX  3     // board silkscreen A3

#if ENABLE_UART1
#  if PIN_UART1_RX < 0 || PIN_UART1_RX > 21 || PIN_UART1_TX < 0 || PIN_UART1_TX > 21
#    error "PIN_UART1_RX / PIN_UART1_TX must be a valid ESP32-C3 GPIO (0..21)"
#  endif
#  if PIN_UART1_RX == PIN_UART1_TX
#    error "PIN_UART1_RX and PIN_UART1_TX must be two different pins"
#  endif
#  if (PIN_UART1_RX >= 11 && PIN_UART1_RX <= 17) || (PIN_UART1_TX >= 11 && PIN_UART1_TX <= 17)
#    error "GPIO11..GPIO17 are wired to the in-package SPI flash and cannot be used"
#  endif
#  if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && \
      (PIN_UART1_RX == 18 || PIN_UART1_RX == 19 || PIN_UART1_TX == 18 || PIN_UART1_TX == 19)
#    error "GPIO18/19 are the native USB pins which this build is using - pick another pin"
#  endif
#  if ENABLE_UART0_BRIDGE &&                                                   \
      (PIN_UART1_RX == PIN_UART0_RX || PIN_UART1_RX == PIN_UART0_TX ||         \
       PIN_UART1_TX == PIN_UART0_RX || PIN_UART1_TX == PIN_UART0_TX)
#    error "PIN_UART1_* overlap with PIN_UART0_* - one pin cannot carry two signals"
#  endif
#endif

// --------------------------------------------------------------- defaults
// NOTE: the USB port is native USB CDC - it completely ignores the baud rate.
// This value only affects the UART0 tap on GPIO20/GPIO21.  It default to a
// fast rate so that mirroring the console there can never throttle the USB
// bridge; change it with AT+BAUD if you attach a slower TTL device.
#define DEFAULT_BAUD      921600
#define DEFAULT_CHANNEL   1       // Wi-Fi channel used by ESP-NOW (1..13)
#define DEFAULT_AUTOPAIR  1       // pair automatically with the first peer found
#define DEFAULT_VERBOSE   1       // print status messages while in data mode

// Bumped whenever a stored default should be replaced on upgrade.
#define SETTINGS_VERSION  2

// --------------------------------------------------------------- protocol
#define ESPNOW_MAX_PAYLOAD  250   // ESP-NOW v1 hard limit
#define ESPNOW_CHUNK        240   // user bytes per radio frame (1 byte header left)

// ------------------------------------------------------------- console buffers
// The arduino core defaults to only 256 bytes for both directions, which a fast
// host (or a fast TTL device on UART0) overruns while the loop is busy with the
// radio.  Larger rings let a burst be absorbed instead of dropped.
#define CONSOLE_RX_BUFFER   4096
#define CONSOLE_TX_BUFFER   2048

// --------------------------------------------------------------- timings
#define HEARTBEAT_INTERVAL_MS  1000
#define PEER_TIMEOUT_MS        4000
#define AUTOPAIR_INTERVAL_MS   2000
#define TX_COALESCE_MS         5     // gather incoming bytes before a radio send
#define ESCAPE_GUARD_MS        1000  // silence required around a "+++" escape

// ------------------------------------------------------------ loop fairness
// A port that refills as fast as we drain it (TX jumpered to RX, or a chatty
// device that echoes) must not be able to keep pumpPort() inside its read loop
// forever: if it did, loop() would never return, the other ports would starve
// and even the USB console - including the ATO that gets you out of trouble -
// would become unreachable.  Cap the bytes handled per call instead.
#define CONSOLE_RX_BUDGET      256
