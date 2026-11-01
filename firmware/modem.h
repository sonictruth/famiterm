// Hayes-style WiFi modem (ported from David Hansel's WiFi Modem, GPLv3; see
// modem.cpp). Input comes from the USB keyboard; everything the modem prints
// or receives goes to the Famicom and Serial (term.h).
#pragma once

// Starts the modem task on core 0 (WiFi setup, then AT commands).
// Call after term.begin() and keyboardBegin().
void modemBegin();
