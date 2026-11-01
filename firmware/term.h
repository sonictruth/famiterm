// The modem's terminal: input from the USB keyboard, output to the Famicom
// screen and mirrored to Serial. Use from the modem task only, never from
// the link task: write() blocks while the Famicom queue is full, which is what
// paces network data to the console's drain rate.
#pragma once
#include <Arduino.h>

class Terminal : public Stream {
public:
  void begin();

  int    available() override;
  int    read() override;
  int    peek() override;
  size_t write(uint8_t c) override;
  using Print::write;

  // Next byte for the Famicom link, or -1 if none. Non-blocking; this is the
  // only member the link task (famicom-link.cpp) may call.
  int famicomRead();
};

extern Terminal term;
