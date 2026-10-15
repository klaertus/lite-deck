// tests/test_golden.cpp: the default rendering of every module, compared with its reference image.
// The comparison tolerates font antialiasing (a Windows update changing the system fonts): at most
// 1% of the pixels may differ by more than 48 on a channel, and at most 0.1% by more than 128
// (ImageDiff in golden.h; the measurements behind it in golden.cpp). The "golden comparator" tests
// below check that a real change still fails. The strip images (now_playing_*, notifications_*)
// have never changed. The key images were regenerated once, for the design system look, after a
// visual review. Never regenerate them to make a change pass.
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include "golden.h"
#include "module_fixtures.h"

using namespace fixtures;

namespace {
void golden(ld::Renderer& r, Rig& rig, const char* name, int w = 112) {
  auto s = draw(*rig.m, r, w, 112);
  check_golden(r, *s, name);
}
const wchar_t* kLongTitle = L"A title far too long to fit in the zone, which scrolls while playing";
}  // namespace

TEST_CASE("golden: clock and date") {
  ld::Renderer r;
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);  // a Wednesday, the longest weekday
  golden(r, *clock_rig(true), "clock_seconds");
  golden(r, *clock_rig(false), "clock_minutes");
  golden(r, *date_rig(), "date_wednesday");
}

TEST_CASE("golden: cpu_load, ram and gpu") {
  ld::Renderer r;
  golden(r, *cpu_load_rig(), "cpu_load_start");
  golden(r, *ram_rig(12.47), "ram_value");
  golden(r, *ram_rig(std::nullopt), "ram_dash");
  golden(r, *gpu_rig("vram", ld::GpuReading{7.81, 64}), "gpu_vram");
  golden(r, *gpu_rig("temp", ld::GpuReading{7.81, 64}), "gpu_temp");
  golden(r, *gpu_rig("temp", std::nullopt), "gpu_dash");
}

TEST_CASE("golden: crypto_gold and weather") {
  ld::Renderer r;
  golden(r, *crypto_rig(Quote::Waiting), "crypto_waiting");
  golden(r, *crypto_rig(Quote::Fresh), "crypto_prices");
  golden(r, *crypto_rig(Quote::Stale), "crypto_stale");
  golden(r, *weather_rig(Sky::Waiting), "weather_waiting");
  golden(r, *weather_rig(Sky::Sunny), "weather_sunny");
  {
    auto rig = weather_rig(Sky::Sunny);
    auto s = draw(*rig->m, r, white_weather());  // the colour now lives in the elements
    check_golden(r, *s, "weather_sunny_white");
  }
  golden(r, *weather_rig(Sky::UnknownCity), "weather_unknown_city");
}

TEST_CASE("golden: stopwatch") {
  ld::Renderer r;
  golden(r, *stopwatch_rig(0), "stopwatch_idle");
  golden(r, *stopwatch_rig(3723000), "stopwatch_paused");
}

TEST_CASE("golden: states and graphs") {
  ld::Renderer r;
  golden(r, *cpu_load_temp_rig(85), "cpu_load_hot");  // the temperature in the warning colour
  golden(r, *gpu_usage_rig(), "gpu_usage");
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  golden(r, *stopwatch_running_rig(3723000), "stopwatch_running");
}

TEST_CASE("golden: seconds below") {
  ld::Renderer r;
  FrozenWallClock frozen(2026, 10, 14, 16, 32, 7);
  golden(r, *clock_rig(true, true), "clock_seconds_below");
  golden(r, *stopwatch_running_rig(3723000, true), "stopwatch_seconds_below");
}

TEST_CASE("golden: now_playing") {
  ld::Renderer r;
  golden(r, *now_playing_rig(std::nullopt), "now_playing_idle", 430);
  golden(r, *now_playing_rig(ld::Track{L"Shake It", L"Metro Station", true, 30, 180}), "now_playing_track", 430);
  golden(r, *now_playing_rig(ld::Track{L"Solo", L"", true}), "now_playing_no_artist", 430);
  golden(r, *now_playing_rig(ld::Track{kLongTitle, L"Artist", false}), "now_playing_paused_long", 430);
}

TEST_CASE("golden: notifications") {
  ld::Renderer r;
  golden(r, *notifications_rig(ld::NotifAccess::Denied), "notifications_denied", 370);
  golden(r, *notifications_rig(ld::NotifAccess::Allowed), "notifications_empty", 370);
  golden(r, *notifications_rig(ld::NotifAccess::Allowed, {{1, L"Discord", L"Alice", L"See you at 8 tonight"}}),
         "notifications_one", 370);
  golden(r,
         *notifications_rig(ld::NotifAccess::Allowed,
                            {{1, L"Outlook", L"Meeting", L"Room 4"},
                             {2, L"Discord", L"Bob", L"A message far too long for the zone, so it scrolls along"}}),
         "notifications_two_long", 370);
}

// The comparator itself, on synthetic keys: a clock-like text and a bar, drawn by the renderer.
namespace {
struct Key {
  std::wstring text = L"13:37";
  float dx = 0, size = 28;
  ld::Rgb color{255, 255, 255};
  bool bar = true;          // a 60 x 4 bar under the text
  bool dot = false;         // an extra 4 x 4 dot in a corner
  int width = 112, height = 112;
};

GoldenImage render(ld::Renderer& r, const Key& k) {
  ld::Surface s(r, k.width, k.height);
  {
    ld::Canvas c = s.canvas();
    c.clear(ld::Rgb{0, 0, 0});
    ld::TextStyle st;
    st.size = k.size;
    st.color = k.color;
    c.text(k.text, ld::RectF{k.dx, 20, 112, 50}, st);
    if (k.bar) c.fill_rect(ld::RectF{26, 80, 60, 4}, ld::Rgb{0, 200, 255});
    if (k.dot) c.fill_rect(ld::RectF{100, 8, 4, 4}, ld::Rgb{255, 80, 0});
  }
  return GoldenImage{k.width, k.height, s.pixels()};
}

// Font antialiasing noise: every partly covered pixel (an edge) moves by up to 40 levels, as the
// font update did (measured max 50, on a few pixels only); solid pixels do not move.
GoldenImage antialias_noise(GoldenImage img) {
  unsigned seed = 12345;
  for (std::size_t i = 0; i < img.bgra.size(); i += 4) {
    const auto [lo, hi] = std::minmax({img.bgra[i], img.bgra[i + 1], img.bgra[i + 2]});
    if (hi == 0 || (hi == 255 && lo == 255)) continue;  // background or solid ink
    seed = seed * 1103515245u + 12345u;
    const int shift = static_cast<int>((seed >> 16) % 81) - 40;
    for (std::size_t c = 0; c < 3; ++c)
      img.bgra[i + c] = static_cast<std::uint8_t>(std::clamp(img.bgra[i + c] + shift, 0, 255));
  }
  return img;
}
}  // namespace

TEST_CASE("golden comparator: identical images pass, exactly") {
  ld::Renderer r;
  const GoldenImage a = render(r, Key{});
  const ImageDiff d = compare_images(a, render(r, Key{}));
  CHECK(d.exact());
  CHECK(d.tolerable());
}

TEST_CASE("golden comparator: antialiasing noise passes, not exactly") {
  ld::Renderer r;
  const GoldenImage a = render(r, Key{});
  const ImageDiff d = compare_images(a, antialias_noise(a));
  CHECK_FALSE(d.exact());
  CHECK(d.changed > 100);  // the noise touches many pixels
  CHECK(d.strong == 0);
  CHECK(d.tolerable());
}

TEST_CASE("golden comparator: a real change fails") {
  ld::Renderer r;
  const GoldenImage a = render(r, Key{});
  const auto fails = [&](const Key& k) {
    const ImageDiff d = compare_images(a, render(r, k));
    INFO("changed " << d.changed << ", differ " << d.differing << ", strong " << d.strong);
    CHECK_FALSE(d.tolerable());
  };
  SUBCASE("text of another colour") { fails(Key{.color = ld::Rgb{255, 140, 0}}); }
  SUBCASE("text a little darker") { fails(Key{.color = ld::Rgb{180, 180, 180}}); }
  SUBCASE("text moved by 2 px") { fails(Key{.dx = 2}); }
  SUBCASE("text moved by 1 px") { fails(Key{.dx = 1}); }
  SUBCASE("text of another size") { fails(Key{.size = 30}); }
  SUBCASE("one digit replaced") { fails(Key{.text = L"13:38"}); }
  SUBCASE("an extra element") { fails(Key{.dot = true}); }
  SUBCASE("a missing element") { fails(Key{.bar = false}); }
  SUBCASE("an image of another size") {
    const ImageDiff d = compare_images(a, render(r, Key{.height = 110}));
    CHECK_FALSE(d.same_size);
    CHECK_FALSE(d.tolerable());
  }
  SUBCASE("noise on top of a real change still fails") {
    CHECK_FALSE(compare_images(a, antialias_noise(render(r, Key{.text = L"13:38"}))).tolerable());
  }
}
