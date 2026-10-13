// src/modules/common/ring.cpp
#include "modules/common/ring.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

namespace ld {
using nlohmann::json;

Rgb scaled(Rgb color, int percent) {
  const int p = std::clamp(percent, 0, 100);
  auto f = [p](std::uint8_t v) { return static_cast<std::uint8_t>((v * p + 50) / 100); };
  return Rgb{f(color.r), f(color.g), f(color.b)};
}

Rgb ring_color(const json& s, const RingStateSpec& state, Rgb default_color, int default_brightness) {
  Rgb color = default_color;
  int brightness = default_brightness;
  if (s.is_object()) {
    if (auto it = s.find(state.color_key); it != s.end() && it->is_string())
      color = parse_color(it->get<std::string>()).value_or(default_color);
    if (auto it = s.find(state.brightness_key); it != s.end() && it->is_number_integer()) brightness = it->get<int>();
  }
  return scaled(color, brightness);
}

json ring_fields(const RingStateSpec& state, Rgb default_color, int default_brightness) {
  return json::array({json{{"key", state.color_key}, {"type", "color"}, {"default", format_color(default_color)},
                           {"label", std::format("Ring color: {}", state.label)}, {"group", "ring"}},
                      json{{"key", state.brightness_key}, {"type", "int"}, {"default", default_brightness},
                           {"min", 0}, {"max", 100}, {"label", std::format("Ring brightness: {} (%)", state.label)},
                           {"group", "ring"}}});
}

}  // namespace ld
