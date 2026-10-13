// tests/test_state_settings.cpp: thresholds with hysteresis and the per-module state settings.
#include <doctest/doctest.h>
#include <cmath>
#include <initializer_list>
#include "modules/common/state_settings.h"
#include "modules/schema.h"

using namespace ld;
using nlohmann::json;

namespace {
const Thresholds kCpu{80, 90, 3};
ElementState feed(std::initializer_list<double> values, const Thresholds& t) {
  ElementState s = ElementState::Normal;
  for (double v : values) s = threshold_state(s, v, t);
  return s;
}
}  // namespace

TEST_CASE("thresholds: up as soon as a threshold is reached") {
  CHECK(feed({79.9}, kCpu) == ElementState::Normal);
  CHECK(feed({80}, kCpu) == ElementState::Warning);
  CHECK(feed({90}, kCpu) == ElementState::Critical);
  CHECK(feed({95}, kCpu) == ElementState::Critical);
  CHECK(feed({50, 92}, kCpu) == ElementState::Critical);  // straight to critical
}

TEST_CASE("thresholds: exactly at a threshold is reached; exactly at threshold minus hysteresis is kept") {
  CHECK(feed({80, 77}, kCpu) == ElementState::Warning);  // 80 - 3: not below
  CHECK(feed({80, 76.9}, kCpu) == ElementState::Normal);
  CHECK(feed({90, 87}, kCpu) == ElementState::Critical);  // 90 - 3
  CHECK(feed({90, 86.9}, kCpu) == ElementState::Warning);
  CHECK(feed({90, 76.9}, kCpu) == ElementState::Normal);  // two steps at once
  CHECK(feed({80, 78, 80, 77.5}, kCpu) == ElementState::Warning);  // no flicker around 80
}

TEST_CASE("thresholds: idle or stale before a value starts from normal; NaN is normal") {
  CHECK(threshold_state(ElementState::Stale, 85, kCpu) == ElementState::Warning);
  CHECK(threshold_state(ElementState::Idle, 50, kCpu) == ElementState::Normal);
  CHECK(threshold_state(ElementState::Critical, std::nan(""), kCpu) == ElementState::Normal);
}

TEST_CASE("threshold fields validate, default and read back, with a prefix") {
  const json fields = threshold_fields("temp_", "Temperature (°C)", Thresholds{75, 85, 3}, 0, 120, true);
  REQUIRE(fields.size() == 3);
  CHECK(fields[0]["key"] == "temp_warn");
  CHECK(fields[1]["key"] == "temp_crit");
  CHECK(fields[2]["key"] == "hysteresis");
  CHECK(fields[0]["group"] == "thresholds");
  const json schema = with_fields(json::parse(R"({"fields":[]})"), fields);
  json out;
  REQUIRE_FALSE(apply_schema(schema, json::object(), out));
  CHECK(out["temp_warn"] == 75.0);
  CHECK(thresholds_from(out, "temp_", Thresholds{}) == Thresholds{75, 85, 3});
  CHECK(apply_schema(schema, json{{"temp_warn", 130}}, out));  // above max
  CHECK(thresholds_from(json{{"temp_warn", 90}, {"temp_crit", 80}}, "temp_", Thresholds{75, 85, 3}).crit == 90);
  CHECK(thresholds_from(json::object(), "", kCpu) == kCpu);  // missing keys: the defaults
  CHECK(threshold_fields("vram_", "VRAM used (%)", Thresholds{90, 97, 3}, 0, 100, false).size() == 2);
}

TEST_CASE("gradient fields validate, default and read back") {
  const json fields = gradient_fields("°C", kTemperatureGradient, -50, 150);
  REQUIRE(fields.size() == 6);
  CHECK(fields[0]["key"] == "cold_at");
  CHECK(fields[1]["key"] == "cold_color");
  CHECK(fields[1]["default"] == "#4fa3ff");
  CHECK(fields[5]["key"] == "hot_color");
  CHECK(fields[5]["group"] == "gradient");
  const json schema = with_fields(json::parse(R"({"fields":[]})"), fields);
  json out;
  REQUIRE_FALSE(apply_schema(schema, json::object(), out));
  CHECK(gradient_from(out, Gradient{}) == kTemperatureGradient);
  json custom = out;
  custom["mid_color"] = "#00ff00";
  custom["mid_at"] = 50;
  const Gradient g = gradient_from(custom, kTemperatureGradient);
  CHECK(g.stops[1].color == Rgb{0, 255, 0});
  CHECK(g.stops[1].at == 50.0);
  CHECK(gradient_from(json{{"cold_color", "nope"}}, kTemperatureGradient).stops[0].color == Rgb{0x4f, 0xa3, 0xff});
}
