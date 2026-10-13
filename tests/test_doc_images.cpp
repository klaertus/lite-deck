// The README's picture of the deck (docs/images/deck.png), composed from the reference images of
// tests/golden: the real key engine's output on fixed, made-up data (no personal data). Also writes
// the strip the page's mock server shows (tests/web/fixtures/strip.png). Manual, to refresh them:
//   build\Debug\lite-deck-tests.exe -tc="doc: deck image" --no-skip
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "core/config.h"
#include "core/types.h"
#include "golden.h"
#include "modules/module.h"
#include "render/renderer.h"

namespace fs = std::filesystem;

namespace {

constexpr int kKey = 112;
constexpr int kStripW = 800;
constexpr int kGapX = (kStripW - 5 * kKey) / 4;  // the keys span the strip, as on the device
constexpr int kGapY = 24;
constexpr int kInset = 24;                       // screen border around keys and strip
constexpr int kPad = 36;                         // bezel around the screen
constexpr int kScreenW = kStripW + 2 * kInset;
constexpr int kScreenH = 3 * kKey + 2 * kGapY + 2 * kInset;
constexpr int kW = kScreenW + 2 * kPad;
constexpr int kKnobY = kPad + kScreenH + 64;
constexpr int kH = kKnobY + 64 + kPad;

struct Picture {
  int w, h;
  std::vector<std::uint8_t> px = std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4, 0);
  void set(int x, int y, ld::Rgb c, double cover = 1.0) {
    if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0) return;
    std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
    auto mix = [&](std::uint8_t& d, std::uint8_t s) { d = static_cast<std::uint8_t>(std::lround(d + (s - d) * cover)); };
    mix(p[0], c.b);
    mix(p[1], c.g);
    mix(p[2], c.r);
    p[3] = std::max<std::uint8_t>(p[3], static_cast<std::uint8_t>(std::lround(255 * cover)));
  }
  // A rectangle with round corners, edges smoothed over 4 x 4 samples per pixel.
  void rounded(int x0, int y0, int rw, int rh, double radius, ld::Rgb c) {
    for (int y = y0; y < y0 + rh; ++y)
      for (int x = x0; x < x0 + rw; ++x) {
        int in = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx) {
            const double px_ = x + (sx + 0.5) / 4, py_ = y + (sy + 0.5) / 4;
            const double cx = std::clamp(px_, x0 + radius, x0 + rw - radius), cy = std::clamp(py_, y0 + radius, y0 + rh - radius);
            in += std::hypot(px_ - cx, py_ - cy) <= radius;
          }
        set(x, y, c, in / 16.0);
      }
  }
  // A disc of radius r (and, with inner > 0, only the ring between inner and r).
  void disc(double cx, double cy, double r, double inner, ld::Rgb c) {
    for (int y = static_cast<int>(cy - r) - 1; y <= cy + r + 1; ++y)
      for (int x = static_cast<int>(cx - r) - 1; x <= cx + r + 1; ++x) {
        int in = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx) {
            const double d = std::hypot(x + (sx + 0.5) / 4 - cx, y + (sy + 0.5) / 4 - cy);
            in += d <= r && d >= inner;
          }
        set(x, y, c, in / 16.0);
      }
  }
  void blit(const std::vector<std::uint8_t>& src, int sw, int sh, int x0, int y0) {
    for (int y = 0; y < sh; ++y)
      std::copy_n(&src[static_cast<std::size_t>(y) * sw * 4], static_cast<std::size_t>(sw) * 4,
                  &px[(static_cast<std::size_t>(y0 + y) * w + x0) * 4]);
  }
};

std::vector<std::uint8_t> golden(ld::Renderer& r, const std::string& name, int w, int h) {
  const auto px = read_png(r, fs::path(LD_TEST_DIR) / "golden" / (name + ".png"), w, h);
  REQUIRE(px);
  REQUIRE(px->size() == static_cast<std::size_t>(w) * h * 4);
  return *px;
}

// A knob's ring as the eye sees the LEDs: its module's first ring colour, at full brightness.
ld::Rgb ring_of(const std::string& module) {
  const ld::ModuleInfo* m = ld::find_module(module);
  REQUIRE(m);
  REQUIRE_FALSE(m->ring_states.empty());
  for (const auto& f : m->settings_schema["fields"]) {
    if (f["key"] != m->ring_states.front().color_key) continue;
    const ld::Rgb c = ld::parse_color(f["default"].get<std::string>()).value();
    const int top = std::max({c.r, c.g, c.b, std::uint8_t{1}});
    auto up = [&](std::uint8_t v) { return static_cast<std::uint8_t>(v * 255 / top); };
    return ld::Rgb{up(c.r), up(c.g), up(c.b)};
  }
  FAIL("no ring colour for " << module);
  return {};
}

}  // namespace

TEST_CASE("doc: deck image" * doctest::skip()) {
  ld::Renderer r;
  // The default layout, with a launcher and a hotkey in place of the sun and the prices.
  const char* keys[] = {"cpu_load_hot", "gpu_usage",     "ram_value",     "gpu_vram",       "stopwatch_running",
                        "clock_seconds", "date_wednesday", "weather_sunny", "launcher_glyph", "hotkey_title"};
  Picture strip{kStripW, kKey};
  strip.blit(golden(r, "now_playing_track", 430, kKey), 430, kKey, 0, 0);
  strip.blit(golden(r, "notifications_one", 370, kKey), 370, kKey, 430, 0);
  write_png(r, fs::path(LD_TEST_DIR) / "web" / "fixtures" / "strip.png", strip.w, strip.h, strip.px);

  Picture deck{kW, kH};
  deck.rounded(0, 0, kW, kH, 28, ld::Rgb{0x16, 0x16, 0x16});
  deck.rounded(kPad, kPad, kScreenW, kScreenH, 10, ld::Rgb{0, 0, 0});
  for (int i = 0; i < 10; ++i) {
    const int x = kPad + kInset + (i % 5) * (kKey + kGapX);
    const int y = kPad + kInset + (i / 5) * (kKey + kGapY);
    deck.rounded(x - 2, y - 2, kKey + 4, kKey + 4, 8, ld::Rgb{0x2a, 0x2a, 0x2a});  // the key's edge
    deck.blit(golden(r, keys[i], kKey, kKey), kKey, kKey, x, y);
  }
  deck.blit(strip.px, kStripW, kKey, kPad + kInset, kPad + kInset + 2 * (kKey + kGapY));
  const ld::Config c = ld::default_config();
  for (int k = 0; k < 4; ++k) {
    const double cx = kPad + kInset + (k + 0.5) * kStripW / 4.0;
    deck.disc(cx, kKnobY, 34, 27, ring_of(c.knobs[static_cast<std::size_t>(k)]->module));
    deck.disc(cx, kKnobY, 26, 0, ld::Rgb{0x22, 0x22, 0x22});
  }
  const fs::path out = fs::path(LD_TEST_DIR) / ".." / "docs" / "images" / "deck.png";
  write_png(r, out, deck.w, deck.h, deck.px);
  CHECK(fs::file_size(out) < 400 * 1024);
  MESSAGE("wrote " << fs::weakly_canonical(out).string() << " (" << fs::file_size(out) / 1024 << " KB)");
}
