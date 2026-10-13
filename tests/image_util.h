// tests/image_util.h: a solid frame to lend to sinks and drivers.
#pragma once
#include <cstdint>
#include <vector>
#include "core/image.h"
#include "core/types.h"

struct SolidImage {
  int width, height;
  std::vector<std::uint8_t> px;
  SolidImage(int w, int h, ld::Rgb c) : width(w), height(h), px(static_cast<std::size_t>(w) * h * 4) {
    for (std::size_t i = 0; i < px.size(); i += 4) {
      px[i] = c.b;
      px[i + 1] = c.g;
      px[i + 2] = c.r;
      px[i + 3] = 255;
    }
  }
  ld::Image image() const { return ld::Image{width, height, width * 4, px}; }
};
