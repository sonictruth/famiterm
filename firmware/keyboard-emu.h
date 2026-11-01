// Family BASIC keyboard emulator (keyboard-emu.cpp): the USB keyboard is
// presented to the Famicom as a Family BASIC keyboard matrix.
#pragma once

// Clears the key matrix and hooks the USB keyboard state. Call after
// keyboardBegin(), with the pins already configured (see setup()).
void keyEmuBegin();

// Tight emulation loop on the calling core; never returns.
[[noreturn]] void keyEmuRun();
