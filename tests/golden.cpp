// tests/golden.cpp
#include "golden.h"
#include <doctest/doctest.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

fs::path golden_dir() { return fs::path(LD_TEST_DIR) / "golden"; }

bool updating() {
  char* v = nullptr;
  std::size_t n = 0;
  const bool on = _dupenv_s(&v, &n, "LD_UPDATE_GOLDEN") == 0 && v && std::strcmp(v, "1") == 0;
  free(v);
  return on;
}

}  // namespace

void write_png(ld::Renderer& r, const fs::path& file, int w, int h, const std::vector<std::uint8_t>& bgra) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::remove(file, ec);
  ComPtr<IWICStream> stream;
  REQUIRE(SUCCEEDED(r.wic()->CreateStream(&stream)));
  REQUIRE(SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)));
  ComPtr<IWICBitmapEncoder> enc;
  REQUIRE(SUCCEEDED(r.wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)));
  REQUIRE(SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
  ComPtr<IWICBitmapFrameEncode> frame;
  REQUIRE(SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)));
  REQUIRE(SUCCEEDED(frame->Initialize(nullptr)));
  REQUIRE(SUCCEEDED(frame->SetSize(w, h)));
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  REQUIRE(SUCCEEDED(frame->SetPixelFormat(&fmt)));
  REQUIRE(fmt == GUID_WICPixelFormat32bppBGRA);  // lossless: the PNG holds exactly these bytes
  REQUIRE(SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))));
  REQUIRE(SUCCEEDED(frame->Commit()));
  REQUIRE(SUCCEEDED(enc->Commit()));
}

// nullopt = missing or unreadable; an empty vector = readable but of another size.
std::optional<std::vector<std::uint8_t>> read_png(ld::Renderer& r, const fs::path& file, int w, int h) {
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(r.wic()->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                &dec)))
    return std::nullopt;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) return std::nullopt;
  UINT fw = 0, fh = 0;
  if (FAILED(frame->GetSize(&fw, &fh))) return std::nullopt;
  if (static_cast<int>(fw) != w || static_cast<int>(fh) != h) return std::vector<std::uint8_t>{};
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(r.wic()->CreateFormatConverter(&conv))) return std::nullopt;
  if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom)))
    return std::nullopt;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
  if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()))) return std::nullopt;
  return out;
}

namespace {

const std::uint8_t* at(const std::vector<std::uint8_t>& px, int width, int x, int y) {
  return &px[(static_cast<std::size_t>(y) * width + x) * 4];
}

std::string describe(const ImageDiff& d) {
  if (!d.same_size) return "size differs";
  if (d.pixels == 0) return "empty image";
  std::ostringstream o;
  o << std::fixed << std::setprecision(2) << d.changed << " of " << d.pixels << " pixel(s) changed, " << d.differing
    << " differ (" << 100.0 * d.differing / d.pixels << "%, limit " << 100 * ImageDiff::kMaxDiffering << "%), "
    << d.strong << " strongly (" << 100.0 * d.strong / d.pixels << "%, limit " << 100 * ImageDiff::kMaxStrong
    << "%), max delta " << d.max_delta;
  return o.str();
}

}  // namespace

namespace {
std::unique_ptr<ld::Surface> draw_impl(ld::Module& m, ld::Renderer& r, const ld::SlotLook* look, int w, int h) {
  auto s = std::make_unique<ld::Surface>(r, w, h);
  {
    ld::Canvas c = s->canvas();
    c.set_look(look);
    c.clear(ld::Rgb{0, 0, 0});
    m.render(c);
  }
  return s;
}
}  // namespace

std::unique_ptr<ld::Surface> draw(ld::Module& m, ld::Renderer& r, int w, int h) { return draw_impl(m, r, nullptr, w, h); }

std::unique_ptr<ld::Surface> draw(ld::Module& m, ld::Renderer& r, const ld::SlotLook& look, int w, int h) {
  return draw_impl(m, r, &look, w, h);
}

void check_golden(ld::Renderer& r, ld::Surface& s, const std::string& name) {
  const std::vector<std::uint8_t> actual = s.pixels();
  const fs::path file = golden_dir() / (name + ".png");
  if (updating()) {
    write_png(r, file, s.width(), s.height(), actual);
    MESSAGE("golden written: " << file.string());
    return;
  }
  const auto expected = read_png(r, file, s.width(), s.height());
  if (!expected) {
    FAIL_CHECK("golden missing: " << file.string() << " (capture it with LD_UPDATE_GOLDEN=1)");
    return;
  }
  // read_png gives an empty vector for a file of another size: compare_images then sees 0 x 0.
  const bool sized = !expected->empty();
  const ImageDiff d = compare_images(GoldenImage{sized ? s.width() : 0, sized ? s.height() : 0, *expected},
                                     GoldenImage{s.width(), s.height(), actual});
  if (d.exact()) return;
  if (d.tolerable()) {
    MESSAGE(name << ": not exact, within tolerance: " << describe(d));
    return;
  }
  const fs::path out = fs::temp_directory_path() / "ld_golden_actual" / (name + ".png");
  write_png(r, out, s.width(), s.height(), actual);
  FAIL_CHECK(name << ": " << describe(d) << " from " << file.string() << "; actual image: " << out.string());
}

// The tolerance (ImageDiff constants). A Windows update (2026-10-14) replaced segoeui.ttf: the
// edges of the text then changed on all 40 reference images (up to 6.4% of the pixels touched),
// but by a delta of at most 50, so at most 0.16% of the pixels differ (delta > 48, disk_io) and
// none strongly (delta > 128). The limits (1% and 0.1%) keep a margin of 6x above that and stay
// below what a real change gives: a text moved by 2 px, a digit replaced, an element of another
// colour, size or an extra one all change tens of pixels by more than 128 or hundreds by more than
// 48 (see the "golden comparator" tests). On a 112 x 112 key, 0.1% is 12 pixels.
ImageDiff compare_images(const GoldenImage& expected, const GoldenImage& actual) {
  ImageDiff d;
  d.pixels = static_cast<std::size_t>(expected.width) * expected.height;
  d.same_size = expected.width == actual.width && expected.height == actual.height &&
                expected.bgra.size() == d.pixels * 4 && actual.bgra.size() == d.pixels * 4;
  if (!d.same_size) return d;
  for (std::size_t i = 0; i < expected.bgra.size(); i += 4) {
    int delta = 0;
    for (std::size_t c = 0; c < 4; ++c)
      delta = std::max(delta, std::abs(int{expected.bgra[i + c]} - int{actual.bgra[i + c]}));
    if (delta == 0) continue;
    ++d.changed;
    d.max_delta = std::max(d.max_delta, delta);
    if (delta > ImageDiff::kDiffDelta) ++d.differing;
    if (delta > ImageDiff::kStrongDelta) ++d.strong;
  }
  return d;
}

Ink ink(ld::Surface& s, int x0, int y0, int x1, int y1) {
  if (x1 < 0) x1 = s.width();
  if (y1 < 0) y1 = s.height();
  const auto px = s.pixels();
  Ink out;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const std::uint8_t* p = at(px, s.width(), x, y);
      if (p[0] + p[1] + p[2] <= 90) continue;
      if (out.count++ == 0) {
        out.left = out.right = x;
        out.top = out.bottom = y;
      }
      out.left = std::min(out.left, x);
      out.right = std::max(out.right, x);
      out.top = std::min(out.top, y);
      out.bottom = std::max(out.bottom, y);
    }
  return out;
}

int saturated(ld::Surface& s, int x0, int y0, int x1, int y1) {
  const auto px = s.pixels();
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const std::uint8_t* p = at(px, s.width(), x, y);
      const auto [lo, hi] = std::minmax({p[0], p[1], p[2]});
      if (hi - lo > 80) ++n;
    }
  return n;
}

bool same_rows(ld::Surface& a, ld::Surface& b, int y0, int y1) {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  const auto pa = a.pixels(), pb = b.pixels();
  const auto row = static_cast<std::ptrdiff_t>(a.width()) * 4;
  return std::equal(pa.begin() + y0 * row, pa.begin() + y1 * row, pb.begin() + y0 * row);
}
