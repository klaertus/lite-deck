#include <doctest/doctest.h>
#include "core/config.h"
#include "fake_loop.h"
#include "layout/deck.h"
#include "device/registry.h"
#include "modules/all_modules.h"
#include "modules/schema.h"
#include "modules/common/wall_clock.h"
#include "module_fixtures.h"

using namespace ld;
using namespace std::chrono_literals;

TEST_CASE("format_clock pads fields") {
  std::tm t{};
  t.tm_hour = 9;
  t.tm_min = 5;
  t.tm_sec = 7;
  CHECK(format_clock(t, true) == L"09:05:07");
  CHECK(format_clock(t, false) == L"09:05");
}

TEST_CASE("until_next_second lands on the next whole second") {
  using sc = std::chrono::system_clock;
  auto base = sc::time_point(std::chrono::seconds(1000));
  CHECK(until_next_second(base + 250ms) == 750ms);
  CHECK(until_next_second(base) == 1000ms);
  CHECK(until_next_second(base + 999ms) == 1ms);
}

TEST_CASE("cpu_busy_percent uses kernel time including idle") {
  CpuTimes a{100, 200, 100};   // kernel includes idle
  CpuTimes b{150, 300, 200};   // +50 idle, +100 kernel, +100 user -> total 200, busy 150
  CHECK(cpu_busy_percent(a, b) == doctest::Approx(75.0));
  CHECK(cpu_busy_percent(a, a) == doctest::Approx(0.0));
  REQUIRE(read_cpu_times());
  CHECK(read_cpu_times()->kernel > 0);
}

TEST_CASE("registry exposes clock and cpu_load on keys only") {
  REQUIRE(find_module("clock"));
  REQUIRE(find_module("cpu_load"));
  CHECK(find_module("clock")->allows(SlotKind::Key));
  CHECK_FALSE(find_module("cpu_load")->allows(SlotKind::Knob));
}

TEST_CASE("default config validates against the real registry") {
  auto r = parse_config(to_json(default_config()), find_module);
  CHECK(r.errors.empty());
  CHECK(r.config.has_value());
}

namespace {
struct CountingSink : DeckSink {
  std::map<int, int> keys;
  void key_image(int slot, const Image&) override { ++keys[slot]; }
  void strip_image(const Image&) override {}
  void rings(std::span<const Rgb>, uint8_t) override {}
};
}  // namespace

TEST_CASE("clock and cpu_load refresh about once per second on a Deck") {
  FakeLoop loop;
  Renderer renderer;
  CountingSink sink;
  Deck deck(loop, renderer, sink, default_device().layout);
  deck.apply(default_config());
  loop.drain();
  int clock_before = sink.keys[5], cpu_before = sink.keys[0];
  loop.advance(3000ms);
  // No upper bound for the clock: FakeLoop time is fake but the clock reads the real time.
  CHECK(sink.keys[5] - clock_before >= 2);
  CHECK(sink.keys[0] - cpu_before >= 2);
  CHECK(sink.keys[0] - cpu_before <= 4);
}

TEST_CASE("until_next_minute lands on the next whole minute") {
  using sc = std::chrono::system_clock;
  auto base = sc::time_point(std::chrono::minutes(1000));
  CHECK(until_next_minute(base + 15s) == 45000ms);
  CHECK(until_next_minute(base) == 60000ms);
}

TEST_CASE("default config places the design system layout") {
  auto c = default_config();
  auto id = [&](int i) { return c.keys[i] ? c.keys[i]->module : std::string("-"); };
  CHECK(id(0) == "cpu_load");
  CHECK(c.keys[0]->settings["show_temperature"] == true);
  CHECK(id(1) == "gpu");
  CHECK(c.keys[1]->settings["mode"] == "usage");
  CHECK(c.keys[1]->settings["show_temperature"] == true);
  CHECK(id(2) == "ram");
  CHECK(id(3) == "gpu");
  CHECK(c.keys[3]->settings["mode"] == "vram");
  CHECK(id(4) == "stopwatch");
  CHECK(id(5) == "clock");
  CHECK(id(6) == "date");
  CHECK(id(7) == "weather");
  CHECK(c.keys[7]->settings["city"] == "");  // no home town: "City?" until configured
  CHECK(id(8) == "sun");
  CHECK(c.keys[8]->settings["verify_days"] == 7);
  CHECK(id(9) == "crypto_gold");
  for (const auto& k : c.keys) {
    REQUIRE(k);
    CHECK(k->elements.empty());  // the module defaults: the design system look
  }
  auto r = parse_config(to_json(c), find_module);
  CHECK(r.errors.empty());
}

// default_config() bypasses apply_schema (the Deck hands settings straight to start()),
// so every default slot must already list every schema key with its default.
TEST_CASE("default config slots are schema-complete") {
  auto c = default_config();
  auto check = [&](const std::optional<SlotConfig>& slot, const std::string& where) {
    if (!slot) return;
    INFO(where << " (" << slot->module << ")");
    const ModuleInfo* info = find_module(slot->module);
    REQUIRE(info);
    nlohmann::json out;
    auto err = apply_schema(info->settings_schema, slot->settings, out);
    CHECK_FALSE(err.has_value());
    CHECK(out == slot->settings);
  };
  for (size_t i = 0; i < c.keys.size(); ++i) check(c.keys[i], "key " + std::to_string(i));
  for (size_t i = 0; i < c.knobs.size(); ++i) check(c.knobs[i], "knob " + std::to_string(i));
  for (size_t i = 0; i < c.strip.size(); ++i) check(c.strip[i].slot, "strip " + std::to_string(i));
}

TEST_CASE("default config places knobs and strip zones") {
  auto c = default_config();
  REQUIRE(c.knobs[0]);
  CHECK(c.knobs[0]->module == "volume");
  CHECK(c.knobs[0]->settings["windows_overlay"] == true);
  REQUIRE(c.knobs[1]);
  CHECK(c.knobs[1]->module == "mic");
  REQUIRE(c.knobs[2]);
  CHECK(c.knobs[2]->module == "media");
  REQUIRE(c.knobs[3]);
  CHECK(c.knobs[3]->module == "seek");
  REQUIRE(c.strip.size() == 2);
  CHECK(c.strip[0].width == 430);
  REQUIRE(c.strip[0].slot);
  CHECK(c.strip[0].slot->module == "now_playing");
  CHECK(c.strip[1].width == 370);
  REQUIRE(c.strip[1].slot);
  CHECK(c.strip[1].slot->module == "notifications");
  CHECK(parse_config(to_json(c), find_module).errors.empty());
}

TEST_CASE("the wall clock can be frozen by a test and given back") {
  const auto t = std::chrono::system_clock::time_point(std::chrono::hours(500000));
  set_wall_clock([t] { return t; });
  CHECK(wall_now() == t);
  set_wall_clock({});
  CHECK(wall_now() != t);
}

TEST_CASE("clock: one time element at the clock size, with or without seconds; no colour setting") {
  fixtures::FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  auto with = fixtures::clock_rig(true);
  auto without = fixtures::clock_rig(false);
  CHECK(fixtures::element_of(*with->m, "time")->text == L"13:37:42");
  CHECK_FALSE(fixtures::element_of(*with->m, "time")->size);  // 28 px by default, whatever the format
  CHECK(fixtures::element_of(*without->m, "time")->text == L"13:37");
  nlohmann::json s;
  REQUIRE_FALSE(apply_schema(clock_module_info().settings_schema, nlohmann::json::object(), s));
  CHECK(s == nlohmann::json{
                 {"seconds", true}, {"seconds_below", false}, {"tap_action", "open"}, {"tap_target", "ms-clock:"}});
}

TEST_CASE("clock seconds below: hours:minutes and the seconds as two elements; nothing below without seconds") {
  fixtures::FrozenWallClock frozen(2026, 10, 14, 16, 32, 7);
  auto below = fixtures::clock_rig(true, true);
  auto c = fixtures::content_of(*below->m);
  REQUIRE(c.size() == 2);
  CHECK(c[0].key == "hours_minutes");
  CHECK(c[0].text == L"16:32");
  CHECK(c[1].key == "seconds");
  CHECK(c[1].text == L"07");
  auto no_seconds = fixtures::clock_rig(false, true);  // seconds off wins: the one-line HH:MM
  auto n = fixtures::content_of(*no_seconds->m);
  REQUIRE(n.size() == 1);
  CHECK(n[0].key == "time");
  CHECK(n[0].text == L"16:32");
  auto line = fixtures::clock_rig(true);
  auto l = fixtures::content_of(*line->m);
  REQUIRE(l.size() == 1);
  CHECK(l[0].key == "time");
  CHECK(l[0].text == L"16:32:07");
}

TEST_CASE("clock seconds below: an older config keeps the one-line time; the setting round-trips") {
  const ModuleInfo info = clock_module_info();
  for (bool seconds : {true, false}) {
    nlohmann::json s;
    REQUIRE_FALSE(apply_schema(info.settings_schema, nlohmann::json{{"seconds", seconds}}, s));
    CHECK(s["seconds"] == seconds);  // untouched
    CHECK(s["seconds_below"] == false);
    nlohmann::json again;
    REQUIRE_FALSE(apply_schema(info.settings_schema, s, again));
    CHECK(again == s);
  }
  nlohmann::json s;
  REQUIRE_FALSE(apply_schema(info.settings_schema, nlohmann::json{{"seconds", true}, {"seconds_below", true}}, s));
  CHECK(s["seconds_below"] == true);
  CHECK(apply_schema(info.settings_schema, nlohmann::json{{"seconds_below", "yes"}}, s));  // a bool only
}
