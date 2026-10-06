#pragma once
#if !defined(ARDUINO) && (defined(__unix__) || defined(__APPLE__))
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "TangNanoVocoder/VocoderClient.h"

namespace tnv {

/**
 * @brief The vocoder link over a serial port on Linux or macOS, e.g. the
 * Tang Nano 20K's USB serial port (/dev/ttyUSB1) - for host builds
 * (tools/host/tnv_speak.cpp). 8N1, raw, 921600 baud by default.
 */
class PosixSerialTransport : public VocoderTransport {
 public:
  ~PosixSerialTransport() override { close(); }

  bool open(const std::string& device, int baud = 921600) {
    close();
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) return false;
    termios tio{};
    if (tcgetattr(fd_, &tio) != 0) return false;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    speed_t speed = baud == 115200 ? B115200 : baud == 460800 ? B460800 : B921600;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSANOW, &tio) != 0) return false;
    tcflush(fd_, TCIOFLUSH);
    return true;
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  void write(const uint8_t* data, size_t len) override {
    while (len > 0) {
      ssize_t n = ::write(fd_, data, len);
      if (n <= 0) return;
      data += n;
      len -= (size_t)n;
    }
    tcdrain(fd_);
  }

  bool read(uint8_t* data, size_t len, uint32_t timeout_ms) override {
    uint32_t start = millis();
    size_t got = 0;
    while (got < len) {
      uint32_t elapsed = millis() - start;
      if (elapsed > timeout_ms) return false;
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, (int)(timeout_ms - elapsed)) <= 0) return false;
      ssize_t n = ::read(fd_, data + got, len - got);
      if (n > 0) got += (size_t)n;
    }
    return true;
  }

  void flushInput() override { tcflush(fd_, TCIFLUSH); }

  uint32_t millis() override {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }

  void delay(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

 protected:
  int fd_ = -1;
};

}  // namespace tnv
#endif
