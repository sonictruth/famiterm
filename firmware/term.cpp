#include "term.h"
#include "usb-keyboard.h"

Terminal term;

static QueueHandle_t famicomQueue;

void Terminal::begin() {
  famicomQueue = xQueueCreate(512, 1);
}

int Terminal::available() { return keyboardAvailable(); }
int Terminal::read()      { return keyboardRead(); }
int Terminal::peek()      { return keyboardPeek(); }

// Waits for queue space while the Famicom is reading, so network data is paced
// to the console. If it stops reading for FAMICOM_TIMEOUT_MS (unplugged, ROM
// not running), output to it is dropped instead, until it drains again --
// otherwise the modem (and Serial) would hang on a full queue.
#define FAMICOM_TIMEOUT_MS 500

size_t Terminal::write(uint8_t c) {
  static bool stalled = false;
  Serial.write(c);
  TickType_t wait = stalled ? 0 : pdMS_TO_TICKS(FAMICOM_TIMEOUT_MS);
  bool sent = xQueueSend(famicomQueue, &c, wait) == pdTRUE;
  if (sent == stalled) {
    stalled = !sent;
    Serial.println(stalled ? "\r\n[famicom] not reading, dropping its output"
                           : "\r\n[famicom] reading again");
  }
  return 1;
}

int Terminal::famicomRead() {
  uint8_t c;
  return xQueueReceive(famicomQueue, &c, 0) == pdTRUE ? c : -1;
}
