// src/device/transport.h
#pragma once
#include <cstdint>
#include <span>

namespace ld {

// Writes one output report (report id first) to the device: the HID link, or a fake in the tests.
class Transport {
public:
  virtual ~Transport() = default;
  virtual bool write(std::span<const std::uint8_t> report) = 0;  // false = device gone
};

}  // namespace ld
