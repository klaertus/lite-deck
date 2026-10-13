// src/modules/common/state_settings.h: per-module thresholds and gradients.
#pragma once
#include <string_view>
#include <nlohmann/json.hpp>
#include "render/look.h"

namespace ld {

struct Thresholds {
  double warn = 0, crit = 0, hysteresis = 3;
  bool operator==(const Thresholds&) const = default;
};

// The state after `value`, given the previous one: Warning from `warn`, Critical from `crit`; a state
// goes back down only once the value is below its threshold minus the hysteresis. Idle and Stale
// count as Normal; NaN gives Normal.
ElementState threshold_state(ElementState previous, double value, const Thresholds& t);

// Schema fields (modules/schema.h) of a module's thresholds: "<prefix>warn" and "<prefix>crit"
// (numbers in [lo, hi]) and, with `with_hysteresis`, "hysteresis" (0 to 20), in the page's
// "thresholds" group ("Warning levels"). `unit` goes in the labels: "Warning at (°C)".
nlohmann::json threshold_fields(std::string_view prefix, std::string_view unit, const Thresholds& defaults, double lo,
                                double hi, bool with_hysteresis);
// The one help text of every "Clear margin" (hysteresis) field.
inline constexpr const char* kClearMarginHint = "Stays in warning until the value is this far back. Avoids flicker.";
// `fields` with the page condition `condition` ({"field", "equals"} or {"field", "in"}) on each.
nlohmann::json shown_if(nlohmann::json fields, const nlohmann::json& condition);
// The thresholds in `settings`: a missing key takes its default; crit below warn is raised to warn.
Thresholds thresholds_from(const nlohmann::json& settings, std::string_view prefix, const Thresholds& defaults);

// Schema fields of a three-stop gradient: cold_at, cold_color, mid_at, mid_color, hot_at, hot_color
// (values in [lo, hi], `unit` in the labels), in the page's "gradient" group.
nlohmann::json gradient_fields(std::string_view unit, const Gradient& defaults, double lo, double hi);
Gradient gradient_from(const nlohmann::json& settings, const Gradient& defaults);

// `schema` with `fields` (an array) appended to schema["fields"].
nlohmann::json with_fields(nlohmann::json schema, const nlohmann::json& fields);

// The scale offered to CPU and GPU temperatures: 40 °C blue, 65 °C white, 90 °C red.
inline constexpr Gradient kTemperatureGradient{{GradientStop{40, Rgb{0x4f, 0xa3, 0xff}},
                                                GradientStop{65, Rgb{0xff, 0xff, 0xff}},
                                                GradientStop{90, Rgb{0xff, 0x5a, 0x3c}}}};

}  // namespace ld
