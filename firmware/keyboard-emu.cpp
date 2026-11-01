/*
 * Family BASIC keyboard emulator for the Famicom expansion port
 * Target: ESP32-S3 (needs USB OTG for host mode), Arduino-ESP32 core + EspUsbHost
 *
 * Selected at boot by holding ESC (see firmware.ino). The USB keyboard
 * (usb-keyboard.cpp) is translated into the Family BASIC keyboard's 9x8
 * matrix protocol.
 *
 * Wiring: see pins.h (shared with the modem link). Through an 8-channel
 * level shifter, ESP side = LV, Famicom = HV. GPIO19/20 are the USB D-/D+
 * pins (keyboard); the USB keyboard needs 5V on VBUS in host mode.
 *
 * ── Protocol (NESdev "Family BASIC Keyboard") ──
 *   $4016 write:  bit0 R = reset to row 0
 *                 bit1 C = column select; row advances on C high->low
 *                 bit2 K = enable (0 -> all lines read as 0)
 *   $4017 read:   bits 1-4 = 4 keys of current row/column, 0 = pressed.
 *   The console inverts these inputs, so on the wire: pressed = HIGH,
 *   released = LOW, disabled = all HIGH.
 *
 * ── Timing ──
 *   Software writes $4016 and reads $4017 a few microseconds later, so the
 *   emulator runs as a tight register-polling loop on core 1 (keyEmuRun(),
 *   never returns). USB host handling runs in its own task on core 0.
 *   Residual jitter is the FreeRTOS tick interrupt on core 1 (~1-2 us).
 */

#include <Arduino.h>
#include "pins.h"
#include "keyboard-emu.h"
#include "usb-keyboard.h"

// $4017 bit number (1..4) -> GPIO mask
static const uint32_t BIT_MASK[5] = {
  0, 1u << PIN_D1, 1u << PIN_D2, 1u << PIN_D3, 1u << PIN_D4
};

// ── Matrix state ───────────────────────────────────────────────────────────
// g_out[row][col] = GPIO mask of D lines to drive HIGH (= keys pressed).
// Rows 0-8 are the keyboard; row 9 is the counter's empty 10th state.
constexpr int ROWS = 10;
static volatile uint32_t g_out[ROWS][2];

// ── Key map: USB HID usage -> Famicom matrix position ─────────────────────
// Matrix per NESdev wiki; verify against it if a key lands wrong.
struct KeyMap { uint8_t usage, row, col, bit; };

static const KeyMap KEYMAP[] = {
  // row 0        col 0: ] [ RETURN F8      col 1: STOP ¥ RSHIFT KANA
  {0x30,0,0,4}, {0x2F,0,0,3}, {0x28,0,0,2}, {0x41,0,0,1},
  {0x45,0,1,4},                 // F12   -> STOP
  {0x48,0,1,4},                 // Pause -> STOP
  {0x31,0,1,3},                 // \     -> ¥
  {0x89,0,1,3},                 // JP ¥ (International3)
  // row 1        col 0: ; : @ F7           col 1: ^ - / _
  {0x33,1,0,4},
  {0x34,1,0,3},                 // '     -> :
  {0x35,1,0,2},                 // `     -> @
  {0x40,1,0,1},
  {0x2E,1,1,4},                 // =     -> ^
  {0x2D,1,1,3}, {0x38,1,1,2},
  {0x87,1,1,1},                 // JP _ (International1)
  // row 2        K L O F6                  0 P , .
  {0x0E,2,0,4}, {0x0F,2,0,3}, {0x12,2,0,2}, {0x3F,2,0,1},
  {0x27,2,1,4}, {0x13,2,1,3}, {0x36,2,1,2}, {0x37,2,1,1},
  // row 3        J U I F5                  8 9 N M
  {0x0D,3,0,4}, {0x18,3,0,3}, {0x0C,3,0,2}, {0x3E,3,0,1},
  {0x25,3,1,4}, {0x26,3,1,3}, {0x11,3,1,2}, {0x10,3,1,1},
  // row 4        H G Y F4                  6 7 V B
  {0x0B,4,0,4}, {0x0A,4,0,3}, {0x1C,4,0,2}, {0x3D,4,0,1},
  {0x23,4,1,4}, {0x24,4,1,3}, {0x19,4,1,2}, {0x05,4,1,1},
  // row 5        D R T F3                  4 5 C F
  {0x07,5,0,4}, {0x15,5,0,3}, {0x17,5,0,2}, {0x3C,5,0,1},
  {0x21,5,1,4}, {0x22,5,1,3}, {0x06,5,1,2}, {0x09,5,1,1},
  // row 6        A S W F2                  3 E Z X
  {0x04,6,0,4}, {0x16,6,0,3}, {0x1A,6,0,2}, {0x3B,6,0,1},
  {0x20,6,1,4}, {0x08,6,1,3}, {0x1D,6,1,2}, {0x1B,6,1,1},
  // row 7        CTR Q ESC F1              2 1 GRPH LSHIFT
  {0x14,7,0,3}, {0x29,7,0,2}, {0x3A,7,0,1},
  {0x1F,7,1,4}, {0x1E,7,1,3},
  // row 8        LEFT RIGHT UP CLR         INS DEL SPACE DOWN
  {0x50,8,0,4}, {0x4F,8,0,3}, {0x52,8,0,2},
  {0x4A,8,0,1},                 // Home      -> CLR HOME
  {0x49,8,1,4},                 // Insert    -> INS
  {0x4C,8,1,3},                 // Delete    -> DEL
  {0x2A,8,1,3},                 // Backspace -> DEL
  {0x2C,8,1,2}, {0x51,8,1,1},
};

// Modifier byte bits -> matrix position
struct ModMap { uint8_t mask, row, col, bit; };
static const ModMap MODMAP[] = {
  {0x01, 7,0,4},   // Left Ctrl   -> CTR
  {0x10, 7,0,4},   // Right Ctrl  -> CTR
  {0x02, 7,1,1},   // Left Shift  -> LSHIFT
  {0x20, 0,1,2},   // Right Shift -> RSHIFT
  {0x04, 7,1,2},   // Left Alt    -> GRPH
  {0x40, 0,1,1},   // Right Alt   -> KANA
};

// Called from the USB task (core 0) with the full key state.
static void rebuildMatrix(uint8_t modifiers, const uint8_t *bitmap) {
  uint32_t m[ROWS][2] = {};

  for (const auto &mm : MODMAP)
    if (modifiers & mm.mask) m[mm.row][mm.col] |= BIT_MASK[mm.bit];

  for (const auto &k : KEYMAP)
    if (bitmap[k.usage >> 3] & (1u << (k.usage & 7))) m[k.row][k.col] |= BIT_MASK[k.bit];

  // Each 32-bit store is atomic; the core-1 loop may briefly see a mix of
  // old and new rows, which is harmless (next scan is consistent).
  for (int row = 0; row < ROWS; row++) {
    g_out[row][0] = m[row][0];
    g_out[row][1] = m[row][1];
  }
}

// ── Setup ──────────────────────────────────────────────────────────────────
// keyboardBegin() must already have been called.
void keyEmuBegin() {
  for (int row = 0; row < ROWS; row++) g_out[row][0] = g_out[row][1] = 0;

  keyboardSetStateHandler(rebuildMatrix);

  disableCore1WDT();   // keyEmuRun() never yields
}

// ── Emulation loop (core 1, never returns) ─────────────────────────────────
void keyEmuRun() {
  uint32_t row = 0;
  uint32_t lastOut1 = 0;
  uint32_t lastDrive = 0xFFFFFFFF;

  for (;;) {
    const uint32_t in  = REG_READ(GPIO_IN_REG);
    const uint32_t out1 = in & M_OUT1;

    if (in & M_OUT0) {
      row = 0;                                    // R: reset to first row
    } else if (lastOut1 && !out1) {
      row = (row == ROWS - 1) ? 0 : row + 1;      // C high->low: next row
    }
    lastOut1 = out1;

    const uint32_t drive = (in & M_OUT2)
                         ? g_out[row][out1 ? 1 : 0]   // enabled: pressed keys high
                         : M_DALL;                    // disabled: all high (reads 0)

    if (drive != lastDrive) {
      REG_WRITE(GPIO_OUT_W1TS_REG, drive);
      REG_WRITE(GPIO_OUT_W1TC_REG, M_DALL & ~drive);
      lastDrive = drive;
    }
  }
}
