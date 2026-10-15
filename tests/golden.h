// tests/golden.h
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "modules/module.h"
#include "render/renderer.h"

// PNG files of 32-bit BGRA pixels, lossless: the reference images and the documentation pictures.
void write_png(ld::Renderer& r, const std::filesystem::path& file, int w, int h, const std::vector<std::uint8_t>& bgra);
// nullopt = missing or unreadable; an empty vector = readable but of another size.
std::optional<std::vector<std::uint8_t>> read_png(ld::Renderer& r, const std::filesystem::path& file, int w, int h);

// Renders `m` on a black w x h surface, as the Deck does.
std::unique_ptr<ld::Surface> draw(ld::Module& m, ld::Renderer& r, int w = 112, int h = 112);

// The same with the slot appearance `look` set on the canvas, as Deck::render_slot does.
std::unique_ptr<ld::Surface> draw(ld::Module& m, ld::Renderer& r, const ld::SlotLook& look, int w = 112, int h = 112);

// A 32-bit BGRA image held in memory, for the golden comparison.
struct GoldenImage {
  int width = 0, height = 0;
  std::vector<std::uint8_t> bgra;  // width * height * 4 bytes
};

// How far `actual` is from `expected`. A pixel's delta is the largest difference of its four
// channels (alpha included).
struct ImageDiff {
  // Tolerance, tuned on the font antialiasing changes of a Windows update (see golden.cpp).
  static constexpr int kDiffDelta = 48;            // a pixel "differs" above this delta
  static constexpr int kStrongDelta = 128;         // a pixel "strongly differs" above this delta
  static constexpr double kMaxDiffering = 0.01;    // at most 1% of the pixels may differ
  static constexpr double kMaxStrong = 0.001;      // and at most 0.1% may strongly differ

  bool same_size = false;
  std::size_t pixels = 0;     // pixels of the expected image
  std::size_t changed = 0;    // pixels with any channel not equal
  std::size_t differing = 0;  // pixels with a delta > kDiffDelta
  std::size_t strong = 0;     // pixels with a delta > kStrongDelta
  int max_delta = 0;

  bool exact() const { return same_size && changed == 0; }
  bool tolerable() const {
    return same_size && pixels > 0 && differing <= kMaxDiffering * static_cast<double>(pixels) &&
           strong <= kMaxStrong * static_cast<double>(pixels);
  }
};
ImageDiff compare_images(const GoldenImage& expected, const GoldenImage& actual);

// Compares `s` with tests/golden/<name>.png, tolerating small antialiasing differences (see
// ImageDiff::tolerable). With LD_UPDATE_GOLDEN=1 in the environment, writes that file instead.
// A match within tolerance but not exact is logged with its statistics. On a failure, saves the
// actual image as %TEMP%\ld_golden_actual\<name>.png and fails with the statistics.
void check_golden(ld::Renderer& r, ld::Surface& s, const std::string& name);

// Lit pixels (r + g + b > 90) in [x0, x1) x [y0, y1); x1 or y1 = -1: up to the edge.
struct Ink {
  int count = 0;
  int left = -1, top = -1, right = -1, bottom = -1;  // bounding box; -1 when count == 0
  int height() const { return count ? bottom - top + 1 : 0; }
};
Ink ink(ld::Surface& s, int x0 = 0, int y0 = 0, int x1 = -1, int y1 = -1);
// Pixels whose channels spread by more than 80 (a colour, not a grey) in [x0, x1) x [y0, y1).
int saturated(ld::Surface& s, int x0, int y0, int x1, int y1);
// True if rows [y0, y1) of two surfaces of the same size are byte for byte identical.
bool same_rows(ld::Surface& a, ld::Surface& b, int y0, int y1);
