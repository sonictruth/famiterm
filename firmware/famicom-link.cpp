// The Famicom link: UART1 and the task that answers the console.
//
//   TX (PIN_D1   = GPIO7, out) -> Famicom P2 D1 ($4017 bit 1), 8N2
//   RX (PIN_OUT1 = GPIO5, in)  <- Famicom OUT1  ($4016 bit 1), 8N1
// Both through the expansion port's level shifter (PROTOCOL.md, "The line").
//
// The console is in charge: once a frame it sends a request and listens for a
// few milliseconds for the reply, counting cycles. One task owns the UART and
// the LinkCore. It sleeps until the UART has a byte, hands what has come to
// the core, and writes the reply, if there is one. Nothing on the way to a
// reply waits for anything: the core answers from what it holds, and the
// terminal stream comes from term's queue without blocking. The task has the
// highest priority on core 1 (WiFi and the modem are on core 0), so the time
// from a request's last byte to its reply is the UART's interrupt, a task
// switch and the core, where the console waits 4 ms.
#include <Arduino.h>
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "famicom-link.h"
#include "link-core.h"
#include "pins.h"
#include "term.h"
#include "usb-keyboard.h"

#define LINK_UART     UART_NUM_1
#define LINK_TX_PIN   PIN_D1
#define LINK_RX_PIN   PIN_OUT1

// The console inverts $4017 bits 1-4 (NESdev "Family BASIC Keyboard"; the
// keyboard emulator in keyboard-emu.cpp relies on it): a pin driven HIGH reads
// 0. So TX is left as it is: it rests high (mark), which the ROM reads as 0,
// and a start bit reads 1, which is what gen-link.py's receiver looks for.
// Build with -DLINK_TX_INVERTED=1 for a board whose shifter inverts as well.
#ifndef LINK_TX_INVERTED
#define LINK_TX_INVERTED 0
#endif
// OUT1 is the CPU's own output, not inverted, and the ROM leaves it at 0, as
// does the controller strobe, which writes $4016 with bit 1 clear. So 0 is
// mark, and the UART inverts its RX.
#ifndef LINK_RX_INVERTED
#define LINK_RX_INVERTED 1
#endif

#define LINK_CORE_ID   1
#define LINK_PRIORITY  (configMAX_PRIORITIES - 2)
#define LINK_STACK     4096
#define RX_BUFFER      512
#define UART_EVENTS    32
// The console is still on its way from sending to listening for ~50 us after
// a request's last stop bit begins; a reply that began before its first look would be
// sampled late (PROTOCOL.md, "Timing"). From the task's waking, so later still.
#define MIN_TURNAROUND_US 100
// A request partway in: the rest is well under a millisecond behind. If none
// comes, what came was noise.
#define PARTIAL_WAIT_MS 2

static size_t takeOutput(uint8_t *out, size_t n) {
  size_t i = 0;
  for (; i < n; i++) {
    int c = term.famicomRead();
    if (c < 0) break;
    out[i] = (uint8_t)c;
  }
  return i;
}

static size_t typedInput(const uint8_t *in, size_t n) {
  return keyboardInject(in, n);
}

static LinkCore core({takeOutput, typedInput});
static QueueHandle_t uartEvents;

// For ATI7, read from the modem task. Plain 32-bit words: a count read while
// it changes is at worst one out, which a readout can live with.
static volatile uint32_t framing, overflows, lastTurnaroundUs, maxTurnaroundUs, lastRequestMs;

static void linkTask(void *) {
  static uint8_t heard[RX_BUFFER];
  static uint8_t reply[LINK_FRAME_MAX];
  uart_event_t event;

  // Whatever the UART heard while the firmware was starting is stale.
  uart_flush_input(LINK_UART);
  xQueueReset(uartEvents);
  for (;;) {
    TickType_t wait = core.partial() ? pdMS_TO_TICKS(PARTIAL_WAIT_MS) : portMAX_DELAY;
    if (xQueueReceive(uartEvents, &event, wait) != pdTRUE) {
      core.flush();
      continue;
    }
    int64_t arrived = esp_timer_get_time();
    if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
      overflows = overflows + 1;
      uart_flush_input(LINK_UART);
      xQueueReset(uartEvents);
      core.flush();
      continue;
    }
    if (event.type == UART_FRAME_ERR || event.type == UART_BREAK) {
      framing = framing + 1;      // a console switched off holds OUT1 low: mark, so
      continue;                   // this is the line, or a polarity setting
    }
    if (event.type != UART_DATA) continue;

    // Everything that has come, not only what this event tells of: the driver
    // drops events when its queue is full, and bytes left behind would lag
    // for good, each request answered when a later one was heard.
    size_t avail = 0;
    uart_get_buffered_data_len(LINK_UART, &avail);
    if (avail > sizeof(heard)) avail = sizeof(heard);
    int n = uart_read_bytes(LINK_UART, heard, avail, 0);
    if (n <= 0) continue;

    size_t len = core.feed(heard, (size_t)n, reply);
    if (len == 0) continue;
    int64_t took = esp_timer_get_time() - arrived;
    if (took < MIN_TURNAROUND_US) esp_rom_delay_us((uint32_t)(MIN_TURNAROUND_US - took));
    // No TX buffer: straight into the 128-byte FIFO, which a reply fits, so
    // its bytes leave back to back, as the ROM's receiver expects.
    uart_write_bytes(LINK_UART, reply, len);
    uint32_t us = (uint32_t)(esp_timer_get_time() - arrived);
    lastTurnaroundUs = us;
    if (us > maxTurnaroundUs) maxTurnaroundUs = us;
    lastRequestMs = millis();
  }
}

void famicomLinkBegin() {
  const uart_config_t config = {
      .baud_rate = LINK_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      // Two stop bits out: the second is the ROM's time to store a byte. The
      // setting is the receiver's too, and the ROM leaves two bits of mark
      // after each byte it sends (gen-link.py).
      .stop_bits = UART_STOP_BITS_2,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_driver_install(LINK_UART, RX_BUFFER, 0, UART_EVENTS, &uartEvents, 0));
  ESP_ERROR_CHECK(uart_param_config(LINK_UART, &config));
  // Before the pins are attached, so that TX never rests at the wrong level.
  uint32_t inverse = (LINK_TX_INVERTED ? UART_SIGNAL_TXD_INV : 0) |
                     (LINK_RX_INVERTED ? UART_SIGNAL_RXD_INV : 0);
  ESP_ERROR_CHECK(uart_set_line_inverse(LINK_UART, inverse));
  ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  // An interrupt for every byte: a request is a few bytes, and what matters is
  // hearing its last one at once.
  ESP_ERROR_CHECK(uart_set_rx_full_threshold(LINK_UART, 1));
  ESP_ERROR_CHECK(uart_set_rx_timeout(LINK_UART, 1));
  xTaskCreatePinnedToCore(linkTask, "famicom-link", LINK_STACK, nullptr, LINK_PRIORITY,
                          nullptr, LINK_CORE_ID);
  Serial.printf("famicom link: UART1 %d baud, TX GPIO%d%s -> P2 D1, RX GPIO%d%s <- OUT1\n",
                LINK_BAUD, LINK_TX_PIN, LINK_TX_INVERTED ? " (inverted)" : "",
                LINK_RX_PIN, LINK_RX_INVERTED ? " (inverted)" : "");
}

void famicomLinkPrintStats() {
  const LinkCounters &c = core.counters();
  uint32_t requests = c.requests, last = lastRequestMs;
  Serial.printf("link: requests=%lu repeats=%lu dropped=%lu noise=%lu hellos=%lu framing=%lu"
                " overflows=%lu turnaround=%luus (max %luus) unacked=%u%s\n",
                (unsigned long)requests, (unsigned long)c.repeats, (unsigned long)c.dropped,
                (unsigned long)c.noise, (unsigned long)c.hellos, (unsigned long)framing,
                (unsigned long)overflows, (unsigned long)lastTurnaroundUs,
                (unsigned long)maxTurnaroundUs, (unsigned)core.unacknowledged(),
                core.helloed() ? "" : " (no hello yet)");
  if (requests == 0)
    Serial.println("  -> nothing heard from the console. A nesterm ROM sends a request every"
                   " frame: check OUT1 -> GPIO5, and LINK_RX_INVERTED if noise is counting.");
  else if (millis() - last > 1000)
    Serial.printf("  -> the console's last request was %lu ms ago: it was reset, switched off"
                  " or is on its splash screen.\n", (unsigned long)(millis() - last));
  else if (c.hellos * 2 > requests && requests > 30)
    Serial.println("  -> the console keeps saying hello: it does not hear the replies. Check"
                   " GPIO7 -> P2 D1, and LINK_TX_INVERTED.");
  else
    Serial.println("  -> link up.");
  Serial.flush();
}
