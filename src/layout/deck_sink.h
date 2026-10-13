// src/layout/deck_sink.h
#pragma once
#include <cstdint>
#include <span>
#include "core/image.h"
#include "core/types.h"

namespace ld {

// Where rendered output goes: the device (DeviceManager -> its driver) or the simulator. Images are
// raw frames of the layout's key or strip size, lent for the call only: copy or encode them before
// returning.
class DeckSink {
public:
  virtual ~DeckSink() = default;
  virtual void key_image(int slot, const Image& image) = 0;
  virtual void strip_image(const Image& image) = 0;
  virtual void rings(std::span<const Rgb> colors, std::uint8_t brightness) = 0;  // one colour per knob
};

}  // namespace ld
