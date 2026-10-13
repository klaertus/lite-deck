// tests/test_golden.cpp: the default rendering of every module, pixel for pixel. The strip images
// (now_playing_*, notifications_*) have never changed. The key images were
// regenerated once, for the design system look, after a visual review. Never regenerate
// them to make a change pass.
#include <doctest/doctest.h>
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
