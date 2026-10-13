// The appearance settings on real modules: element sizes and weights, the slot font, the fit to the
// key. The default rendering is pinned by test_golden.cpp.
#include <doctest/doctest.h>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include "golden.h"
#include "modules/common/key_look.h"
#include "module_fixtures.h"

using namespace fixtures;

namespace {
ld::SlotLook look_with(const char* key, std::optional<int> size, std::optional<int> weight = std::nullopt) {
  ld::SlotLook look;
  look.elements[key] = ld::ElementOverride{size, weight};
  return look;
}
std::size_t element_count(const char* module) { return ld::find_module(module)->elements.size(); }
// The ElementSpec `key` of the registered module `module`.
const ld::ElementSpec& spec_of(const char* module, std::string_view key) {
  for (const ld::ElementSpec& e : ld::find_module(module)->elements)
    if (key == e.key) return e;
  throw std::runtime_error("no element " + std::string(key) + " in " + module);
}
}  // namespace

TEST_CASE("every module declares sound elements: unique keys, labels, defaults within bounds") {
  // The icon flag must match the kind once a module is on the engine (every module
  // is now).
  const std::set<std::string> on_engine{"cpu_load", "cpu_temp",    "ram", "gpu",         "clock",
                                        "date",     "stopwatch",   "weather", "crypto_gold", "sun",
                                        "now_playing", "notifications"};
  for (const ld::ModuleInfo& m : ld::module_registry()) {
    std::set<std::string> keys;
    for (const ld::ElementSpec& e : m.elements) {
      INFO(m.id << "." << e.key);
      CHECK(keys.insert(e.key).second);
      CHECK(std::string(e.label).size() > 0);
      if (on_engine.contains(m.id)) CHECK(e.icon == (e.kind == ld::ElementKind::Icon));
      if (e.kind == ld::ElementKind::Graph) {
        CHECK((e.row == ld::Row::Middle || e.row == ld::Row::Bottom));
        continue;  // a graph has no size or weight
      }
      CHECK(e.min_size >= 8);
      CHECK(e.min_size <= e.size);
      CHECK(e.size <= static_cast<float>(e.max_size));
      CHECK(e.max_size <= 90);
      CHECK((e.weight == 400 || e.weight == 600 || e.weight == 700));
    }
  }
  CHECK(element_count("clock") == 3);
  CHECK(element_count("cpu_load") == 4);
  CHECK(element_count("cpu_temp") == 3);
  CHECK(element_count("ram") == 3);
  CHECK(element_count("gpu") == 5);
  CHECK(element_count("date") == 3);
  CHECK(element_count("weather") == 4);
  CHECK(element_count("crypto_gold") == 5);
  CHECK(element_count("stopwatch") == 5);
  CHECK(element_count("sun") == 7);
  CHECK(element_count("volume") == 0);  // knobs draw nothing
  for (const ld::ModuleInfo& m : ld::module_registry()) {
    INFO(m.id);
    CHECK(ld::element_spec_errors(m.elements).empty());  // the registry's own check
  }
  CHECK(ld::module_registry().size() == 22);  // none was rejected
}

TEST_CASE("a module with unsound element specs is rejected when the registry is built") {
  auto with = [](std::vector<ld::ElementSpec> elements) {
    ld::ModuleInfo m;
    m.id = "third_party";
    m.elements = std::move(elements);
    return m;
  };
  const ld::ElementSpec good{.key = "value", .label = "Value", .size = 20, .weight = 700, .min_size = 8, .max_size = 40,
                             .role = ld::TextRole::Value};
  CHECK(ld::element_spec_errors(std::vector{good}).empty());
  ld::ElementSpec no_key = good;
  no_key.key = nullptr;
  ld::ElementSpec inverted = good;
  inverted.min_size = 50;
  ld::ElementSpec weight0 = good;
  weight0.weight = 0;
  ld::ElementSpec icon_flag = good;
  icon_flag.kind = ld::ElementKind::Icon;  // without .icon = true
  icon_flag.key = "icon";
  icon_flag.role = ld::TextRole::None;
  ld::ElementSpec graph_role = good;
  graph_role.kind = ld::ElementKind::Graph;
  graph_role.key = "graph";
  ld::ElementSpec nan_size = good;
  nan_size.size = std::numeric_limits<float>::quiet_NaN();
  for (const auto& bad : std::vector<std::vector<ld::ElementSpec>>{
           {no_key}, {good, good}, {inverted}, {weight0}, {icon_flag}, {graph_role}, {nan_size}}) {
    CHECK(ld::element_spec_errors(bad).size() == 1);
    CHECK(ld::sound_modules({with(bad)}).empty());
  }
  ld::ElementSpec graph = graph_role;  // a graph has no weight and no role, and that is fine
  graph.role = ld::TextRole::None;
  graph.weight = 0;
  graph.min_size = 9;
  graph.max_size = 8;
  const auto kept = ld::sound_modules({with({good, graph}), with({inverted})});
  REQUIRE(kept.size() == 1);
  CHECK(kept[0].elements.size() == 2);
}

TEST_CASE("a larger value size draws taller text, a lighter weight less ink") {
  ld::Renderer r;
  auto rig = cpu_load_rig();
  auto base = draw(*rig->m, r);
  auto big = draw(*rig->m, r, look_with("value", 60));
  auto light = draw(*rig->m, r, look_with("value", std::nullopt, 400));
  CHECK(ink(*big).count > 2 * ink(*base).count);  // 60 px instead of 34: far more ink
  CHECK(ink(*light).count < ink(*base).count);
}

TEST_CASE("an enlarged time is shrunk to fit the key instead of overflowing") {
  ld::Renderer r;
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  auto rig = clock_rig(true);
  // What overflowing looks like: the same text at 60 px, cut on both sides of the key.
  ld::Surface cut(r, 112, 112);
  {
    ld::Canvas c = cut.canvas();
    c.clear({0, 0, 0});
    ld::TextStyle st;
    st.size = 60;
    st.weight = 700;
    c.text(L"13:37:42", ld::RectF{0, 0, 112, 112}, st);
  }
  REQUIRE(ink(cut, 0, 0, 1, 112).count > 0);
  auto fitted = draw(*rig->m, r, look_with("time", 60));
  CHECK(ink(*fitted, 0, 0, 1, 112).count == 0);
  CHECK(ink(*fitted, 111, 0, 112, 112).count == 0);
  CHECK(ink(*fitted).count > 0);
}

TEST_CASE("the weather icon keeps its colour emoji whatever the slot font") {
  ld::Renderer r;
  auto rig = weather_rig(Sky::Sunny);  // white text: a monochrome sun would be grey
  ld::SlotLook symbol = white_weather();
  symbol.font = L"Segoe UI Symbol";  // has its own monochrome ☀
  symbol.italic = true;
  auto plain = draw(*rig->m, r, white_weather());
  auto styled = draw(*rig->m, r, symbol);
  CHECK(same_rows(*plain, *styled, 0, 34));       // the icon area: identical
  CHECK(saturated(*styled, 0, 0, 112, 34) > 20);  // and in colour
  CHECK_FALSE(same_rows(*plain, *styled, 38, 112));  // the temperature: another font, slanted
}

TEST_CASE("the slot font reaches every text of a key module") {
  ld::Renderer r;
  ld::SlotLook consolas;
  consolas.font = L"Consolas";
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  for (auto* rig : {clock_rig(true).release(), date_rig().release(), ram_rig(12.47).release(),
                    stopwatch_rig(3723000).release(), crypto_rig(Quote::Fresh).release()}) {
    std::unique_ptr<Rig> owner(rig);
    auto plain = draw(*owner->m, r);
    auto styled = draw(*owner->m, r, consolas);
    CHECK_FALSE(same_rows(*plain, *styled, 0, 112));
  }
}

TEST_CASE("strip modules declare their elements") {
  CHECK(element_count("now_playing") == 5);
  CHECK(element_count("notifications") == 6);
}

TEST_CASE("strip: a hidden title is not drawn, the artist stays") {
  ld::Renderer r;
  auto rig = now_playing_rig(ld::Track{L"Shake It", L"Metro Station", true, 30, 180});
  ld::SlotLook look;
  look.elements["title"].visible = false;
  auto base = draw(*rig->m, r, 430, 112);
  auto hidden = draw(*rig->m, r, look, 430, 112);
  CHECK(ink(*hidden, 50, 15, 430, 55).count < ink(*base, 50, 15, 430, 55).count / 4);
  CHECK(same_rows(*base, *hidden, 60, 112));  // the artist and the bar did not move
}

TEST_CASE("strip: the idle texts take the palette's idle colour; a fixed colour wins elsewhere") {
  ld::Renderer r;
  auto idle = now_playing_rig(std::nullopt);
  ld::SlotLook red;
  red.states.idle = ld::Rgb{255, 0, 0};
  auto s = draw(*idle->m, r, red, 430, 112);
  CHECK(saturated(*s, 0, 0, 430, 112) > 50);  // red text
  auto rig = notifications_rig(ld::NotifAccess::Allowed, {{1, L"Discord", L"Alice", L"See you"}});
  ld::SlotLook app;
  app.elements["app"].color = ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{255, 255, 255}};
  auto plain = draw(*rig->m, r, 370, 112);
  auto white = draw(*rig->m, r, app, 370, 112);
  CHECK(saturated(*white, 0, 0, 200, 56) < saturated(*plain, 0, 0, 200, 56));  // the blue app name is white
}

TEST_CASE("strip: access denied keeps the text colour unless the user picks the state colour") {
  ld::Renderer r;
  auto rig = notifications_rig(ld::NotifAccess::Denied);
  ld::SlotLook state;
  state.elements["status"].color = ld::ColorChoice{ld::ColorMode::State, {}};
  state.states.stale = ld::Rgb{0, 0, 255};
  auto plain = draw(*rig->m, r, 370, 112);
  auto blue = draw(*rig->m, r, state, 370, 112);
  CHECK(saturated(*plain, 40, 0, 370, 112) == 0);  // white text (the ⚠ glyph may be coloured: skipped)
  CHECK(saturated(*blue, 40, 0, 370, 112) > 50);
}

TEST_CASE("strip: the progress bar can be hidden and recoloured") {
  ld::Renderer r;
  auto rig = now_playing_rig(ld::Track{L"Shake It", L"Metro Station", true, 90, 180});
  ld::SlotLook none;
  none.elements["bar"].visible = false;
  auto base = draw(*rig->m, r, 430, 112);
  auto hidden = draw(*rig->m, r, none, 430, 112);
  CHECK(ink(*base, 50, 96, 430, 102).count > 0);
  CHECK(ink(*hidden, 50, 96, 430, 102).count == 0);
}

TEST_CASE("now_playing no longer has a font_size setting") {
  const auto& fields = ld::find_module("now_playing")->settings_schema["fields"];
  for (const auto& f : fields) CHECK(f["key"] != "font_size");
  nlohmann::json out;
  CHECK_FALSE(ld::apply_schema(ld::find_module("now_playing")->settings_schema,
                               nlohmann::json{{"scroll_speed", 2}, {"color", "#ffffff"}, {"font_size", 0}}, out));
  CHECK_FALSE(out.contains("font_size"));  // an old config still loads; the key is dropped
}

TEST_CASE("now_playing: a larger title draws taller text; the artist follows unless set") {
  ld::Renderer r;
  auto rig = now_playing_rig(ld::Track{L"Shake It", L"Metro Station", true, 30, 180});
  auto base = draw(*rig->m, r, 430, 112);
  auto big = draw(*rig->m, r, look_with("title", 44), 430, 112);
  CHECK(ink(*big, 40, 0, 430, 56).count > ink(*base, 40, 0, 430, 56).count);
  ld::SlotLook both = look_with("title", 44);
  both.elements["artist"] = ld::ElementOverride{14, std::nullopt};
  auto small_artist = draw(*rig->m, r, both, 430, 112);
  CHECK(ink(*small_artist, 40, 60, 430, 95).count < ink(*big, 40, 60, 430, 95).count);
}

TEST_CASE("idle texts shrink to fit a narrow zone") {
  ld::Renderer r;
  auto idle = now_playing_rig(std::nullopt);
  auto np = draw(*idle->m, r, 140, 112);
  CHECK(ink(*np).count > 0);
  CHECK(ink(*np, 128, 0, 140, 112).count == 0);  // nothing past the text area (12 px margin)
  auto empty = notifications_rig(ld::NotifAccess::Allowed);
  auto nt = draw(*empty->m, r, 80, 112);
  CHECK(ink(*nt).count > 0);
  CHECK(ink(*nt, 0, 0, 1, 112).count == 0);
  CHECK(ink(*nt, 79, 0, 80, 112).count == 0);
}

TEST_CASE("notifications: the message size follows its element, the slot font reaches every text") {
  ld::Renderer r;
  auto rig = notifications_rig(ld::NotifAccess::Allowed, {{1, L"Discord", L"Alice", L"See you"}});
  auto base = draw(*rig->m, r, 370, 112);
  auto big = draw(*rig->m, r, look_with("message", 40), 370, 112);
  CHECK(ink(*big, 0, 56, 370, 112).count > ink(*base, 0, 56, 370, 112).count);
  ld::SlotLook consolas;
  consolas.font = L"Consolas";
  auto styled = draw(*rig->m, r, consolas, 370, 112);
  CHECK_FALSE(same_rows(*base, *styled, 0, 56));
  CHECK_FALSE(same_rows(*base, *styled, 56, 112));
}

TEST_CASE("notifications: a huge counter and age shrink instead of overflowing") {
  ld::Renderer r;
  auto rig = notifications_rig(ld::NotifAccess::Allowed, {{1, L"Discord", L"Alice", L"See you"}});
  ld::SlotLook look = look_with("count", 40);
  look.elements["age"] = ld::ElementOverride{40, std::nullopt};
  auto img = draw(*rig->m, r, look, 120, 112);
  CHECK(ink(*img, 119, 0, 120, 112).count == 0);
}

TEST_CASE("every key module draws in the six-pixel margins of the template, never on the bezel") {
  ld::Renderer r;
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  for (auto* rig : {clock_rig(true).release(), date_rig().release(), ram_rig(31.94).release(),
                    stopwatch_rig(86399000).release(), crypto_rig(Quote::Fresh).release(),
                    weather_rig(Sky::Sunny).release(), cpu_load_temp_rig(100).release()}) {
    std::unique_ptr<Rig> owner(rig);
    auto s = draw(*owner->m, r);
    CHECK(ink(*s).count > 0);
    CHECK(ink(*s, 0, 0, 4, 112).count == 0);  // 6 px margins; 2 px left for antialiasing
    CHECK(ink(*s, 108, 0, 112, 112).count == 0);
  }
}

TEST_CASE("the temperature header turns amber at 80 °C and red at 90 °C") {
  ld::Renderer r;
  auto warm = draw(*cpu_load_temp_rig(85)->m, r);
  auto hot = draw(*cpu_load_temp_rig(95)->m, r);
  auto mild = draw(*cpu_load_temp_rig(50)->m, r);
  auto amber_or_red = [](ld::Surface& s) {  // red channel far above blue, in the top-right corner
    const auto px = s.pixels();
    int n = 0;
    for (int y = 4; y < 24; ++y)
      for (int x = 60; x < 106; ++x) {
        const std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * 112 + x) * 4;
        if (p[2] > 200 && p[0] < 90) ++n;
      }
    return n;
  };
  CHECK(amber_or_red(*mild) == 0);  // teal
  CHECK(amber_or_red(*warm) > 5);
  CHECK(amber_or_red(*hot) > 5);
}

TEST_CASE("weather: City? is small idle text; the stopwatch indicator fits the top row, running or paused") {
  ld::Renderer r;
  auto lost = weather_rig(Sky::UnknownCity);
  const auto st = element_of(*lost->m, "status");
  REQUIRE(st);
  CHECK(st->text == L"City?");
  CHECK(st->state == ld::ElementState::Idle);
  CHECK_FALSE(element_of(*lost->m, "temp"));
  const Ink city = ink(*draw(*lost->m, r));
  CHECK(city.height() < 20);  // 17 px, not the 36 px value size
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  for (auto* rig : {stopwatch_running_rig(3723000).release(), stopwatch_rig(3723000).release()}) {
    std::unique_ptr<Rig> owner(rig);
    auto s = draw(*owner->m, r);
    const Ink mark = ink(*s, 0, 0, 40, 30);  // the indicator, top left
    CHECK(mark.count > 0);
    CHECK(mark.top >= 4);
    CHECK(mark.bottom < 24);
    CHECK(mark.left >= 6);
  }
}

TEST_CASE("every text of every key module has a role; icons, graphs and the strip's elements have none") {
  using R = ld::TextRole;
  const std::map<std::string, R> expected{
      {"clock.time", R::LongValue},       {"clock.hours_minutes", R::Value},  {"clock.seconds", R::Secondary},
      {"cpu_load.value", R::Value},       {"cpu_load.label", R::Label},       {"cpu_load.temp", R::Secondary},
      {"cpu_temp.label", R::Label},       {"cpu_temp.value", R::Value},
      {"ram.label", R::Label},            {"ram.value", R::Value},            {"ram.unit", R::Secondary},
      {"gpu.label", R::Label},            {"gpu.value", R::Value},            {"gpu.unit", R::Secondary},
      {"gpu.temp", R::Secondary},
      {"date.weekday", R::Secondary},     {"date.day", R::Value},             {"date.month", R::Secondary},
      {"stopwatch.label", R::Label},      {"stopwatch.time", R::LongValue},   {"stopwatch.state", R::Symbol},
      {"stopwatch.hours_minutes", R::Value}, {"stopwatch.seconds", R::Secondary},
      {"weather.temp", R::Value},         {"weather.range", R::Secondary},    {"weather.status", R::Secondary},
      {"sun.status", R::Secondary},       {"sun.city_hint", R::Secondary},    {"sun.symbol1", R::Symbol},
      {"sun.line1", R::TwoLine},          {"sun.symbol2", R::Symbol},         {"sun.line2", R::TwoLine},
      {"sun.online", R::Symbol},
      {"crypto_gold.status", R::Secondary}, {"crypto_gold.symbol1", R::Symbol}, {"crypto_gold.line1", R::TwoLine},
      {"crypto_gold.symbol2", R::Symbol},   {"crypto_gold.line2", R::TwoLine},
      {"net_speed.label", R::Label},      {"net_speed.up", R::Secondary},     {"net_speed.value", R::Value},
      {"net_speed.unit", R::Secondary},
      {"ping.label", R::Label},           {"ping.target", R::Secondary},      {"ping.value", R::Value},
      {"ping.loss", R::Secondary},
      {"disk.label", R::Label},           {"disk.write", R::Secondary},       {"disk.value", R::Value},
      {"disk.unit", R::Secondary},        {"disk.rate_unit", R::Secondary},
      {"http_sensor.label", R::Label},    {"http_sensor.value", R::Value},    {"http_sensor.unit", R::Secondary},
      {"launcher.title", R::Value},       {"launcher.label", R::Label},
      {"hotkey.title", R::Value},         {"hotkey.label", R::Label}};
  std::size_t seen = 0;
  for (const ld::ModuleInfo& m : ld::module_registry()) {
    const bool key = m.allows(ld::SlotKind::Key);
    for (const ld::ElementSpec& e : m.elements) {
      const std::string id = m.id + "." + e.key;
      INFO(id);
      const bool text = e.kind == ld::ElementKind::Text && !e.icon;
      if (key && text) {
        CHECK(e.role != R::None);
        auto it = expected.find(id);
        REQUIRE(it != expected.end());
        CHECK(e.role == it->second);
        ++seen;
      } else {
        CHECK(e.role == R::None);  // icons, graphs, the strip
      }
    }
  }
  CHECK(seen == expected.size());
}

TEST_CASE("a value size set for the role enlarges the CPU percentage, not the clock") {
  FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  ld::SlotLook look;
  look.roles[ld::TextRole::Value].size = 50;
  ld::Renderer r;
  {
    ld::Surface s(r, 112, 112);
    ld::Canvas c = s.canvas();
    c.set_look(&look);
    CHECK(c.resolve(spec_of("cpu_load", "value")).size == 50.f);
    CHECK(c.resolve(spec_of("cpu_load", "label")).size == 17.f);
    CHECK(c.resolve(spec_of("clock", "time")).size == 28.f);
    c.set_look(nullptr);
  }
  auto cpu = cpu_load_rig();
  auto base = draw(*cpu->m, r);
  auto big = draw(*cpu->m, r, look);
  CHECK(ink(*big).count > ink(*base).count);  // 50 px instead of 36
  auto clk = clock_rig(false);
  auto plain = draw(*clk->m, r);
  auto same = draw(*clk->m, r, look);
  CHECK(same_rows(*plain, *same, 0, 112));  // the time is a LongValue: untouched
}

TEST_CASE("seconds below: hours:minutes is a main value in the middle, the seconds a smaller line centred below") {
  for (const char* module : {"clock", "stopwatch"}) {
    INFO(module);
    const ld::ElementSpec& time = spec_of(module, "time");
    const ld::ElementSpec& hm = spec_of(module, "hours_minutes");
    const ld::ElementSpec& sec = spec_of(module, "seconds");
    CHECK(hm.role == ld::TextRole::Value);
    CHECK(hm.size == 36.f);  // the size of the other main values, not the long value's 28
    CHECK(hm.row == ld::Row::Middle);
    CHECK(hm.color_mode == time.color_mode);
    CHECK(hm.color == time.color);
    CHECK(sec.row == ld::Row::Bottom);
    CHECK(sec.align == ld::HAlign::Center);
    CHECK(sec.size < hm.size);
    CHECK(sec.role == ld::TextRole::Secondary);
    const ld::ElementSpec a = ld::seconds_spec(time, ld::SecondsStyle::SameColor);
    const ld::ElementSpec b = ld::seconds_spec(time, ld::SecondsStyle::Grey);
    CHECK(a.color == time.color);
    CHECK(a.color_mode == time.color_mode);
    CHECK(b.color == ld::kSecondaryGrey);
    CHECK(b.color_mode == ld::ColorMode::Fixed);
    CHECK(b.keeps_color);
    const ld::ElementSpec& chosen = ld::kSecondsStyle == ld::SecondsStyle::SameColor ? a : b;
    CHECK(sec.color == chosen.color);
    CHECK(sec.color_mode == chosen.color_mode);
    CHECK(sec.weight == chosen.weight);
  }
  ld::SlotLook look;
  look.roles[ld::TextRole::Value].size = 50;
  ld::Renderer r;
  ld::Surface s(r, 112, 112);
  ld::Canvas c = s.canvas();
  c.set_look(&look);
  CHECK(c.resolve(spec_of("clock", "hours_minutes")).size == 50.f);  // the value role sizes it
  CHECK(c.resolve(spec_of("clock", "time")).size == 28.f);
  c.set_look(nullptr);
}

TEST_CASE("seconds below draws hours:minutes larger than the one-line time and the seconds centred below it") {
  ld::Renderer r;
  FrozenWallClock frozen(2026, 10, 14, 16, 32, 7);
  auto check_layout = [&](ld::Module& below, ld::Module& line) {
    auto two = draw(below, r);
    auto one = draw(line, r);
    const Ink hm = ink(*two, 0, 24, 112, 76), sec = ink(*two, 0, 76, 112, 112), time = ink(*one, 0, 24, 112, 76);
    REQUIRE(hm.count > 0);
    REQUIRE(sec.count > 0);
    CHECK(hm.height() > time.height());  // 36 px instead of 28
    CHECK(sec.height() < hm.height());
    CHECK(sec.top > hm.bottom);
    CHECK(std::abs((sec.left + sec.right) - (hm.left + hm.right)) <= 4);  // both centred
    CHECK(ink(*one, 0, 76, 112, 112).count == 0);  // the one-line time leaves the bottom row empty
  };
  check_layout(*clock_rig(true, true)->m, *clock_rig(true)->m);
  auto running = stopwatch_running_rig(3723000, true);
  check_layout(*running->m, *stopwatch_running_rig(3723000)->m);
  auto two = draw(*running->m, r);
  auto one = draw(*stopwatch_running_rig(3723000)->m, r);
  CHECK(same_rows(*one, *two, 0, 24));  // the ► stays top-left, unchanged
}
