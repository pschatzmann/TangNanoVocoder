#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include <SPI.h>

#include "TangNanoVocoder/VocoderClient.h"

namespace tnv {

/**
 * @brief The vocoder link over a UART: the FPGA's header UART (pins 25 RX,
 * 26 TX) or, from a PC, the board's USB serial port. 8N1, 921600 baud
 * (the bitstream's setting) - call serial.begin(921600, ...) first.
 */
class SerialTransport : public VocoderTransport {
 public:
  explicit SerialTransport(Stream& serial) : s_(serial) {}

  void write(const uint8_t* data, size_t len) override {
    s_.write(data, len);
    s_.flush();
  }

  bool read(uint8_t* data, size_t len, uint32_t timeout_ms) override {
    uint32_t start = ::millis();
    size_t got = 0;
    while (got < len) {
      if (s_.available()) {
        data[got++] = (uint8_t)s_.read();
      } else if (::millis() - start > timeout_ms) {
        return false;
      } else {
        yield();
      }
    }
    return true;
  }

  void flushInput() override {
    while (s_.available()) s_.read();
  }

  uint32_t millis() override { return ::millis(); }
  void delay(uint32_t ms) override { ::delay(ms); }

 protected:
  Stream& s_;
};

/**
 * @brief The vocoder link over SPI: mode 0, the FPGA is the slave on pins
 * 27 SCLK, 28 MOSI, 29 MISO, 30 CS (the same as TangNanoFaust). Every byte
 * clocked returns the status; replies come after the request, in a new
 * chip-select frame (the FPGA loads the next reply byte while CS is high).
 * Call spi.begin(...) first.
 */
class SPITransport : public VocoderTransport {
 public:
  SPITransport(SPIClass& spi, int cs_pin, uint32_t clock_hz = 4000000)
      : spi_(spi), cs_(cs_pin), settings_(clock_hz, MSBFIRST, SPI_MODE0) {
    pinMode(cs_, OUTPUT);
    digitalWrite(cs_, HIGH);
  }

  void write(const uint8_t* data, size_t len) override {
    spi_.beginTransaction(settings_);
    digitalWrite(cs_, LOW);
    for (size_t i = 0; i < len; i++) spi_.transfer(data[i]);
    digitalWrite(cs_, HIGH);
    spi_.endTransaction();
    delayMicroseconds(5);  // the reply is queued a few FPGA clocks after the request
  }

  bool read(uint8_t* data, size_t len, uint32_t timeout_ms) override {
    // the request's reply bytes (or the status, when none is pending)
    spi_.beginTransaction(settings_);
    digitalWrite(cs_, LOW);
    for (size_t i = 0; i < len; i++) {
      data[i] = spi_.transfer(0x00);
      delayMicroseconds(2);  // time for the FPGA to fetch the next reply byte (e.g. an SDRAM word)
    }
    digitalWrite(cs_, HIGH);
    spi_.endTransaction();
    return true;
  }

  uint32_t millis() override { return ::millis(); }
  void delay(uint32_t ms) override { ::delay(ms); }

 protected:
  SPIClass& spi_;
  int cs_;
  SPISettings settings_;
};

}  // namespace tnv
#endif
