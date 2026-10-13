// tests/test_jpeg.cpp: JpegEncoder and SurfacePixels against the reference pipeline of Task 1.
#include <doctest/doctest.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <vector>
#include "n4pro_reference.h"
#include "render/jpeg.h"
#include "render/renderer.h"

using namespace ld;

namespace {
void paint(Surface& s) {
  Canvas c = s.canvas();
  c.clear(Rgb{0, 0, 0});
  c.fill_rect(RectF{0, 0, c.width() / 4, c.height() / 2}, Rgb{250, 250, 250});
  c.fill_rect(RectF{c.width() / 2, c.height() / 2, c.width() / 2, c.height() / 2}, Rgb{20, 140, 230});
}
// A decoded JPEG: its size and its pixels as 32bppBGRA.
struct Decoded {
  UINT w = 0, h = 0;
  std::vector<std::uint8_t> bgra;
  Rgb at(UINT x, UINT y) const {
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    return Rgb{bgra[i + 2], bgra[i + 1], bgra[i]};
  }
};

Decoded decode(Renderer& r, const std::vector<std::uint8_t>& jpeg) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICStream> stream;
  REQUIRE(SUCCEEDED(r.wic()->CreateStream(&stream)));
  REQUIRE(SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size()))));
  ComPtr<IWICBitmapDecoder> dec;
  REQUIRE(SUCCEEDED(r.wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)));
  ComPtr<IWICBitmapFrameDecode> frame;
  REQUIRE(SUCCEEDED(dec->GetFrame(0, &frame)));
  ComPtr<IWICFormatConverter> conv;
  REQUIRE(SUCCEEDED(r.wic()->CreateFormatConverter(&conv)));
  REQUIRE(SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)));
  Decoded d;
  REQUIRE(SUCCEEDED(conv->GetSize(&d.w, &d.h)));
  d.bgra.resize(static_cast<std::size_t>(d.w) * d.h * 4);
  REQUIRE(SUCCEEDED(conv->CopyPixels(nullptr, d.w * 4, static_cast<UINT>(d.bgra.size()), d.bgra.data())));
  return d;
}

bool close_to(Rgb a, Rgb b) {
  auto close = [](int x, int y) { return x - y < 24 && y - x < 24; };
  return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b);
}
}  // namespace

TEST_CASE("JpegEncoder gives the bytes of the reference pipeline, rotated or not") {
  Renderer r;
  JpegEncoder enc;
  for (const auto& [w, h, q, rot] : {std::tuple{112, 112, 85, true}, std::tuple{800, 112, 80, true},
                                     std::tuple{96, 96, 90, false}, std::tuple{120, 100, 85, true}}) {
    INFO(w << "x" << h << " q" << q << (rot ? " rotated" : ""));
    Surface s(r, w, h);
    paint(s);
    const auto expected = ref::jpeg(r, s, q, rot);
    std::vector<std::uint8_t> got;
    {
      SurfacePixels px(s);
      CHECK(px.image().width == w);
      CHECK(px.image().height == h);
      CHECK(px.image().stride >= w * 4);
      got = enc.encode(px.image(), q, rot ? Orientation::Rotate180 : Orientation::None);
    }
    CHECK(got == expected);
    CHECK(s.encode_jpeg(q, rot) == expected);  // the surface's own encode goes the same way
  }
}

TEST_CASE("JpegEncoder honours the stride and refuses a short buffer") {
  Renderer r;
  Surface s(r, 112, 112);
  paint(s);
  const std::vector<std::uint8_t> tight = s.pixels();
  const int stride = 112 * 4 + 16;  // padded rows, as a WIC lock may give
  std::vector<std::uint8_t> padded(static_cast<std::size_t>(stride) * 112, 0xAB);
  for (int y = 0; y < 112; ++y)
    std::copy_n(tight.begin() + y * 112 * 4, 112 * 4, padded.begin() + static_cast<std::ptrdiff_t>(y) * stride);
  JpegEncoder enc;
  CHECK(enc.encode(Image{112, 112, stride, padded}, 85, Orientation::Rotate180) == enc.encode(Image{112, 112, 112 * 4, tight}, 85, Orientation::Rotate180));
  CHECK_THROWS_AS(enc.encode(Image{112, 112, 112 * 4, std::span<const std::uint8_t>(tight).first(100)}, 85, Orientation::Rotate180),
                  std::invalid_argument);
  CHECK_THROWS_AS(enc.encode(Image{0, 112, 0, tight}, 85, Orientation::Rotate180), std::invalid_argument);
  CHECK_THROWS_AS(enc.encode(Image{112, 112, 100, tight}, 85, Orientation::Rotate180), std::invalid_argument);  // stride < width * 4
}

TEST_CASE("JpegEncoder keeps one scratch bitmap per frame size, at most four") {
  JpegEncoder enc;
  const std::vector<std::uint8_t> px(800 * 112 * 4, 0);
  for (int i = 0; i < 3; ++i) {
    enc.encode(Image{112, 112, 112 * 4, px}, 85, Orientation::Rotate180);
    enc.encode(Image{800, 112, 800 * 4, px}, 80, Orientation::Rotate180);
  }
  CHECK(enc.scratch_count() == 2);
  for (int w : {10, 20, 30, 40, 50}) enc.encode(Image{w, 10, w * 4, px}, 85, Orientation::None);
  CHECK(enc.scratch_count() == JpegEncoder::kMaxScratch);
}

TEST_CASE("a surface draws again after its pixels were lent") {
  Renderer r;
  Surface s(r, 20, 20);
  {
    Canvas c = s.canvas();
    c.clear(Rgb{0, 255, 0});
  }
  {
    SurfacePixels px(s);
    REQUIRE(px.image().bgra.size() >= 20 * 20 * 4);
    CHECK(px.image().bgra[0] == 0);    // B
    CHECK(px.image().bgra[1] == 255);  // G
    CHECK(px.image().bgra[2] == 0);    // R
  }
  {
    Canvas c = s.canvas();
    c.clear(Rgb{255, 255, 255});
  }
  CHECK(s.pixel(5, 5) == Rgb{255, 255, 255});
}

TEST_CASE("JpegEncoder turns a frame by 90 or 270 degrees and swaps its size, or mirrors it") {
  // 60 x 30, black with a red 20 x 10 block in the top-left corner.
  Renderer r;
  Surface s(r, 60, 30);
  {
    Canvas c = s.canvas();
    c.clear(Rgb{0, 0, 0});
    c.fill_rect(RectF{0, 0, 20, 10}, Rgb{255, 0, 0});
  }
  const Rgb red{255, 0, 0}, black{0, 0, 0};
  JpegEncoder enc;
  SurfacePixels px(s);
  struct Case {
    Orientation o;
    UINT w, h;
    UINT red_x, red_y;      // the middle of the block once turned
    UINT black_x, black_y;  // where the block was
  };
  // Clockwise: (x, y) -> (h - 1 - y, x) for 90, (y, w - 1 - x) for 270.
  for (const Case& c : {Case{Orientation::Rotate90, 30, 60, 25, 10, 5, 10},
                        Case{Orientation::Rotate270, 30, 60, 5, 50, 5, 10},
                        Case{Orientation::Rotate180, 60, 30, 50, 25, 10, 5},
                        Case{Orientation::FlipHorizontal, 60, 30, 50, 5, 10, 5},
                        Case{Orientation::FlipVertical, 60, 30, 10, 25, 10, 5},
                        Case{Orientation::None, 60, 30, 10, 5, 50, 25}}) {
    INFO("orientation " << static_cast<int>(c.o));
    const Decoded d = decode(r, enc.encode(px.image(), 90, c.o));
    CHECK(d.w == c.w);
    CHECK(d.h == c.h);
    CHECK(close_to(d.at(c.red_x, c.red_y), red));
    CHECK(close_to(d.at(c.black_x, c.black_y), black));
  }
}

TEST_CASE("Orientation maps to the WIC transforms") {
  CHECK(wic_transform(Orientation::None) == WICBitmapTransformRotate0);
  CHECK(wic_transform(Orientation::Rotate90) == WICBitmapTransformRotate90);
  CHECK(wic_transform(Orientation::Rotate180) == WICBitmapTransformRotate180);
  CHECK(wic_transform(Orientation::Rotate270) == WICBitmapTransformRotate270);
  CHECK(wic_transform(Orientation::FlipHorizontal) == WICBitmapTransformFlipHorizontal);
  CHECK(wic_transform(Orientation::FlipVertical) == WICBitmapTransformFlipVertical);
  CHECK(swaps_size(Orientation::Rotate90));
  CHECK(swaps_size(Orientation::Rotate270));
  CHECK_FALSE(swaps_size(Orientation::Rotate180));
  CHECK_FALSE(swaps_size(Orientation::FlipHorizontal));
}

TEST_CASE("BandJpegEncoder gives JpegEncoder's upright bytes for opaque frames, one stream and one band reused") {
  Renderer r;
  JpegEncoder ref_enc;
  BandJpegEncoder enc;
  std::vector<std::uint8_t> got;
  // Large then small: the kept stream holds a longer JPEG from before, which must not leak in.
  for (const auto& [w, h, q] : {std::tuple{800, 112, 80}, std::tuple{112, 112, 85}, std::tuple{120, 100, 85},
                                std::tuple{96, 7, 90}, std::tuple{800, 112, 80}}) {
    INFO(w << "x" << h << " q" << q);
    Surface s(r, w, h);
    paint(s);
    SurfacePixels px(s);
    enc.encode(px.image(), q, got);
    CHECK(got == ref_enc.encode(px.image(), q, Orientation::None));
  }
}

TEST_CASE("BandJpegEncoder honours the stride and refuses a short buffer") {
  Renderer r;
  Surface s(r, 112, 112);
  paint(s);
  const std::vector<std::uint8_t> tight = s.pixels();
  const int stride = 112 * 4 + 16;
  std::vector<std::uint8_t> padded(static_cast<std::size_t>(stride) * 112, 0xAB);
  for (int y = 0; y < 112; ++y)
    std::copy_n(tight.begin() + y * 112 * 4, 112 * 4, padded.begin() + static_cast<std::ptrdiff_t>(y) * stride);
  BandJpegEncoder enc;
  std::vector<std::uint8_t> a, b;
  enc.encode(Image{112, 112, stride, padded}, 85, a);
  enc.encode(Image{112, 112, 112 * 4, tight}, 85, b);
  CHECK(a == b);
  CHECK_THROWS_AS(enc.encode(Image{112, 112, 112 * 4, std::span<const std::uint8_t>(tight).first(100)}, 85, a), std::invalid_argument);
  CHECK_THROWS_AS(enc.encode(Image{0, 112, 0, tight}, 85, a), std::invalid_argument);
  CHECK_THROWS_AS(enc.encode(Image{112, 112, 100, tight}, 85, a), std::invalid_argument);
}
