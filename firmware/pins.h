// Famicom expansion-port wiring, shared by the modem link (famicom-link.cpp,
// UART1) and the Family BASIC keyboard emulator (keyboard-emu.cpp). All pins
// are < 32 so a single GPIO register covers them.
//
//   Famicom exp pin | Signal | Dir        | ESP32-S3 | Keyboard emu      | Modem
//   ----------------+--------+------------+----------+-------------------+------
//   12              | OUT0   | Fam -> ESP | GPIO4    | row reset         | -
//   11              | OUT1   | Fam -> ESP | GPIO5    | column / row adv. | UART RX
//   10              | OUT2   | Fam -> ESP | GPIO6    | keyboard enable   | -
//    7              | P2 D1  | ESP -> Fam | GPIO7    | $4017 bit 1       | UART TX
//    6              | P2 D2  | ESP -> Fam | GPIO15   | $4017 bit 2       | -
//    5              | P2 D3  | ESP -> Fam | GPIO16   | $4017 bit 3       | -
//    4              | P2 D4  | ESP -> Fam | GPIO17   | $4017 bit 4       | -
#pragma once
#include <stdint.h>
#include "soc/gpio_reg.h"

constexpr int PIN_OUT0 = 4;
constexpr int PIN_OUT1 = 5;
constexpr int PIN_OUT2 = 6;
constexpr int PIN_D1   = 7;
constexpr int PIN_D2   = 15;
constexpr int PIN_D3   = 16;
constexpr int PIN_D4   = 17;

constexpr uint32_t M_OUT0 = 1u << PIN_OUT0;
constexpr uint32_t M_OUT1 = 1u << PIN_OUT1;
constexpr uint32_t M_OUT2 = 1u << PIN_OUT2;
constexpr uint32_t M_DALL = (1u << PIN_D1) | (1u << PIN_D2) | (1u << PIN_D3) | (1u << PIN_D4);
