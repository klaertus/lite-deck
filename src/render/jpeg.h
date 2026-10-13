// src/render/jpeg.h: JPEG encoding of raw frames (WIC). A driver encodes for its device with it, the
// web UI for its previews. On the thread that uses it (COM initialised).
#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "core/image.h"

namespace ld {

// How a frame is turned before it is encoded, for a screen mounted turned or mirrored. Rotations are
// clockwise; a 90° or 270° turn swaps the encoded width and height.
enum class Orientation { None, Rotate90, Rotate180, Rotate270, FlipHorizontal, FlipVertical };

// The WIC transform for `o`.
WICBitmapTransformOptions wic_transform(Orientation o);
// True when `o` swaps the width and height of the frame (90° and 270°).
bool swaps_size(Orientation o);

// `source` (32bppPBGRA, width x height) -> `orientation` -> 24bppBGR -> baseline JPEG at quality / 100.
// Throws std::runtime_error when WIC fails.
std::vector<std::uint8_t> encode_jpeg_source(IWICImagingFactory* wic, IWICBitmapSource* source, int width, int height,
                                             int quality, Orientation orientation);

// Encodes lent Images: the same bytes as Surface::encode_jpeg for the same pixels. Keeps one scratch
// bitmap per frame size (at most kMaxScratch, oldest dropped), so steady frames allocate no pixel
// buffer; the WIC factory is created at the first encode.
class JpegEncoder {
public:
  static constexpr std::size_t kMaxScratch = 4;
  // Throws std::invalid_argument when the span is shorter than width, height and stride say, and
  // std::runtime_error when WIC fails.
  std::vector<std::uint8_t> encode(const Image& image, int quality, Orientation orientation);
  std::size_t scratch_count() const { return scratch_.size(); }

private:
  struct Scratch {
    int w = 0, h = 0;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  };
  IWICImagingFactory* wic();
  IWICBitmap* scratch(int w, int h);
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
  std::vector<Scratch> scratch_;
};

// Encodes lent Images upright with the least memory moved per encode (the web previews, re-encoded
// as fast as an animated deck draws): no scratch bitmap and no format converter, the rows go to the
// frame in bands of kBand, turned into 24bppBGR in one small kept buffer, and the JPEG is written to
// one kept stream, rewound each time. Opaque pixels give JpegEncoder's bytes (premultiplied BGRA over
// black is its colour channels). On the thread that uses it (COM initialised).
class BandJpegEncoder {
public:
  static constexpr int kBand = 16;
  // The JPEG goes into `out`, whose capacity is reused. Throws like JpegEncoder::encode.
  void encode(const Image& image, int quality, std::vector<std::uint8_t>& out);

private:
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
  Microsoft::WRL::ComPtr<IStream> stream_;
  std::vector<std::uint8_t> band_;
};

}  // namespace ld
