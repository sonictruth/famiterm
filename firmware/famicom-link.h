// The Famicom link on the ESP32-S3's UART1 (PROTOCOL.md): link-core.* on the
// expansion port's two wires. Replaces the clocked, interrupt-driven protocol
// of the first versions; the wires are the same.
#pragma once

// UART1 on the pins in pins.h, and the link task on core 1. Call after
// term.begin(): the task takes the console's output from term's queue.
void famicomLinkBegin();

// ATI7: the link's counts, to Serial.
void famicomLinkPrintStats();
