// USB keyboard on the ESP32-S3's native USB (OTG) port, via the EspUsbHost
// library. Typed characters are queued for the modem (see usb-keyboard.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Starts the USB host (background task on core 0).
void keyboardBegin();

// Number of queued characters.
int keyboardAvailable();

// Next typed character (removed / left in the queue), or -1 if none.
// Non-blocking.
int keyboardRead();
int keyboardPeek();

// Queue characters as if typed (the Famicom's TERM WRITE): how many fitted.
// Non-blocking.
size_t keyboardInject(const uint8_t *chars, size_t n);

// True while a USB device is attached / while the keyboard's ESC is held.
bool keyboardConnected();
bool keyboardEscDown();

// Raw key-state handler: called from the USB task (core 0) on every report
// with the modifier byte and the 256-bit HID usage bitmap (bit n = usage n
// down). Once set, typed characters are no longer queued.
typedef void (*KeyboardStateHandler)(uint8_t modifiers, const uint8_t *bitmap);
void keyboardSetStateHandler(KeyboardStateHandler handler);
