// fami-modem (ESP32-S3 firmware): a WiFi modem for the Famicom. The USB
// keyboard types AT commands (ATDhost:port dials a telnet connection, +++
// returns to command mode); the modem's echo, results and everything the
// remote sends are shown on the Famicom, over the framed UART link in
// PROTOCOL.md, and mirrored to Serial. Build with `make firmware`.
//
// Holding ESC on the keyboard at power-up instead turns the ESP32-S3 into a
// Family BASIC keyboard emulator (keyboard-emu.*) until the next reset.
//
// pins.h         -- expansion-port pin assignments (shared by both modes)
// famicom-link.* -- the Famicom link: UART1 and its task (link-core.* is the
//                   protocol, link-proto.h its numbers, shared with the ROM)
// keyboard-emu.* -- Family BASIC keyboard emulator
// usb-keyboard.* -- USB keyboard input (EspUsbHost)
// term.*         -- the modem's terminal: keyboard in, Famicom + Serial out
// modem.*        -- the modem (task on core 0)
#include <Arduino.h>
#include "pins.h"
#include "famicom-link.h"
#include "keyboard-emu.h"
#include "usb-keyboard.h"
#include "term.h"
#include "modem.h"

// Boot-mode selection: hold ESC on the USB keyboard while powering up to run
// the Family BASIC keyboard emulator instead of the modem. USB enumeration
// takes a moment, so wait up to ESC_DETECT_MS for a device, then give the
// keyboard ESC_SETTLE_MS to report a held ESC.
constexpr uint32_t ESC_DETECT_MS = 6000;
constexpr uint32_t ESC_SETTLE_MS = 3000;

static bool escHeldAtBoot() {
  Serial.println("hold ESC for Family BASIC keyboard mode...");
  uint32_t start = millis();
  uint32_t connectedAt = 0;
  for (;;) {
    if (keyboardEscDown()) return true;
    uint32_t now = millis();
    if (keyboardConnected()) {
      if (!connectedAt) connectedAt = now;
      if (now - connectedAt >= ESC_SETTLE_MS) return false;
    } else if (now - start >= ESC_DETECT_MS) {
      return false;
    }
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);

  keyboardBegin();

  pinMode(PIN_OUT0, INPUT);   // no pull-ups: the shifter provides them
  pinMode(PIN_OUT1, INPUT);
  pinMode(PIN_OUT2, INPUT);
  pinMode(PIN_D1, OUTPUT);
  pinMode(PIN_D2, OUTPUT);
  pinMode(PIN_D3, OUTPUT);
  pinMode(PIN_D4, OUTPUT);

  if (escHeldAtBoot()) {
    Serial.println("mode: Family BASIC keyboard emulator");
    Serial.flush();
    REG_WRITE(GPIO_OUT_W1TS_REG, M_DALL);   // start "disabled": all lines high
    keyEmuBegin();
    keyEmuRun();   // never returns
  } else {
    Serial.println("mode: modem, build " __DATE__ " " __TIME__);
    Serial.flush();
    // All four lines high: what the keyboard mode starts from, and on D1 the
    // UART's mark until famicomLinkBegin() takes the pin (see LINK_TX_INVERTED
    // in famicom-link.cpp).
    REG_WRITE(GPIO_OUT_W1TS_REG, M_DALL);
    term.begin();
    famicomLinkBegin();   // TX = PIN_D1, RX = PIN_OUT1
    modemBegin();
  }
}

// Everything runs in tasks: the link on core 1 (famicom-link.cpp), the modem
// and USB on core 0.
void loop() {
  vTaskDelay(portMAX_DELAY);
}
