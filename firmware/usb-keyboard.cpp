#include <Arduino.h>
#include "usb-keyboard.h"
#include "EspUsbHost.h"

// EspUsbHost runs in its own task on core 0 (loop() is on core 1) and queues
// each typed character; the modem task reads them through `term`. USB events
// (device connect/disconnect, errors) are logged to Serial only,
// prefixed "[usb]".
static EspUsbHost    usb;
static QueueHandle_t keyQueue;
static volatile int  connectedCount = 0;
static volatile bool escDown = false;
static volatile KeyboardStateHandler stateHandler = nullptr;

constexpr uint8_t HID_ESC       = 0x29;
constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_DELETE    = 0x4C;
constexpr uint8_t ASCII_DEL     = 0x7F;

void keyboardBegin() {
  keyQueue = xQueueCreate(64, 1);
  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &d) {
    Serial.print("[usb] connected: ");
    espUsbHostPrint(d);
    connectedCount = connectedCount + 1;
  });
  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &d) {
    Serial.print("[usb] disconnected: ");
    espUsbHostPrint(d);
    if (connectedCount > 0) connectedCount = connectedCount - 1;
    escDown = false;
  });
  usb.onKeyboard([](const EspUsbHostKeyboardEvent &e) {
    if (stateHandler || !e.pressed) return;
    // Backspace/Delete send DEL (^?), the default Unix tty erase character, like xterm
    uint8_t c = (e.keycode == HID_BACKSPACE || e.keycode == HID_DELETE) ? ASCII_DEL : e.ascii;
    if (!c) return;
    if (xQueueSend(keyQueue, &c, 0) != pdTRUE) Serial.println("[usb] key queue full, dropped");
  });
  usb.onKeyboardState([](const EspUsbHostKeyboardState &s) {
    escDown = s.isDown(HID_ESC);
    KeyboardStateHandler h = stateHandler;
    if (h) h(s.modifiers, s.bitmap);
  });
  EspUsbHostConfig cfg;
  cfg.taskCore = 0;
  if (usb.begin(cfg))
    Serial.println("[usb] host started, waiting for a keyboard on the OTG port");
  else
    Serial.printf("[usb] host start failed: %s\n", usb.lastErrorName());
}

int keyboardAvailable() {
  return uxQueueMessagesWaiting(keyQueue);
}

int keyboardRead() {
  uint8_t c;
  return xQueueReceive(keyQueue, &c, 0) == pdTRUE ? c : -1;
}

int keyboardPeek() {
  uint8_t c;
  return xQueuePeek(keyQueue, &c, 0) == pdTRUE ? c : -1;
}

size_t keyboardInject(const uint8_t *chars, size_t n) {
  size_t i = 0;
  while (i < n && xQueueSend(keyQueue, &chars[i], 0) == pdTRUE) i++;
  return i;
}

bool keyboardConnected() {
  return connectedCount > 0;
}

bool keyboardEscDown() {
  return escDown;
}

void keyboardSetStateHandler(KeyboardStateHandler handler) {
  stateHandler = handler;
}
