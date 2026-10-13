// src/core/image.h
#pragma once
#include <cstdint>
#include <span>

namespace ld {

// A raw frame lent for the duration of one call: premultiplied BGRA (opaque after a clear), `stride`
// bytes per row (at least width * 4). Copy or encode it before returning; never keep the span.
struct Image {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::span<const std::uint8_t> bgra;
};

}  // namespace ld
