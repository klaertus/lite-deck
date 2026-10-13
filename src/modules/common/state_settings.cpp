// src/modules/common/state_settings.cpp
#include "modules/common/state_settings.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace ld {
using nlohmann::json;

ElementState threshold_state(ElementState previous, double v, const Thresholds& t) {
  if (std::isnan(v)) return ElementState::Normal;
  int level = previous == ElementState::Critical ? 2 : previous == ElementState::Warning ? 1 : 0;
  if (level == 2 && v < t.crit - t.hysteresis) level = 1;
  if (level == 1 && v < t.warn - t.hysteresis) level = 0;
  const int reached = v >= t.crit ? 2 : v >= t.warn ? 1 : 0;
  level = std::max(level, reached);
  return level == 2 ? ElementState::Critical : level == 1 ? ElementState::Warning : ElementState::Normal;
}

json threshold_fields(std::string_view prefix, std::string_view unit, const Thresholds& d, double lo, double hi,
                      bool with_hysteresis) {
  const std::string p(prefix);
  json f = json::array();
  f.push_back(json{{"key", p + "warn"}, {"type", "number"}, {"default", d.warn}, {"min", lo}, {"max", hi},
                   {"label", std::format("Warning at ({})", unit)}, {"group", "thresholds"}});
  f.push_back(json{{"key", p + "crit"}, {"type", "number"}, {"default", d.crit}, {"min", lo}, {"max", hi},
                   {"label", std::format("Critical at ({})", unit)}, {"group", "thresholds"}});
  if (with_hysteresis)
    f.push_back(json{{"key", "hysteresis"}, {"type", "number"}, {"default", d.hysteresis}, {"min", 0}, {"max", 20},
                     {"label", "Clear margin"}, {"hint", kClearMarginHint}, {"group", "thresholds"}});
  return f;
}

json shown_if(json fields, const json& condition) {
  for (json& f : fields) f["show_if"] = condition;
  return fields;
}

namespace {
double number(const json& s, const std::string& key, double def) {
  if (!s.is_object()) return def;
  auto it = s.find(key);
  return it != s.end() && it->is_number() ? it->get<double>() : def;
}
Rgb color(const json& s, const std::string& key, Rgb def) {
  if (!s.is_object()) return def;
  auto it = s.find(key);
  if (it == s.end() || !it->is_string()) return def;
  return parse_color(it->get<std::string>()).value_or(def);
}
constexpr const char* kStopNames[] = {"cold", "mid", "hot"};
constexpr const char* kStopLabels[] = {"Cold", "Mid", "Hot"};
}  // namespace

Thresholds thresholds_from(const json& s, std::string_view prefix, const Thresholds& d) {
  const std::string p(prefix);
  Thresholds t{number(s, p + "warn", d.warn), number(s, p + "crit", d.crit), number(s, "hysteresis", d.hysteresis)};
  if (t.crit < t.warn) t.crit = t.warn;
  if (!(t.hysteresis >= 0)) t.hysteresis = 0;
  return t;
}

json gradient_fields(std::string_view unit, const Gradient& d, double lo, double hi) {
  json f = json::array();
  for (std::size_t i = 0; i < 3; ++i) {
    const std::string n = kStopNames[i];
    f.push_back(json{{"key", n + "_at"}, {"type", "number"}, {"default", d.stops[i].at}, {"min", lo}, {"max", hi},
                     {"label", std::format("{} at ({})", kStopLabels[i], unit)}, {"group", "gradient"}});
    f.push_back(json{{"key", n + "_color"}, {"type", "color"}, {"default", format_color(d.stops[i].color)},
                     {"label", std::format("{} color", kStopLabels[i])}, {"group", "gradient"}});
  }
  return f;
}

Gradient gradient_from(const json& s, const Gradient& d) {
  Gradient g = d;
  for (std::size_t i = 0; i < 3; ++i) {
    const std::string n = kStopNames[i];
    g.stops[i].at = number(s, n + "_at", d.stops[i].at);
    g.stops[i].color = color(s, n + "_color", d.stops[i].color);
  }
  return g;
}

json with_fields(json schema, const json& fields) {
  for (const json& f : fields) schema["fields"].push_back(f);
  return schema;
}

}  // namespace ld
