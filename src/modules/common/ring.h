// src/modules/common/ring.h: knob ring colours per state.
#pragma once
#include <nlohmann/json.hpp>
#include "modules/module.h"

namespace ld {

// `color` at `percent` % (0-100, clamped), rounded per channel.
Rgb scaled(Rgb color, int percent);
// The ring of `state`: its colour setting at its brightness setting; a missing or invalid setting
// takes the given default.
Rgb ring_color(const nlohmann::json& settings, const RingStateSpec& state, Rgb default_color, int default_brightness);
// The two schema fields of `state` (colour, then brightness 0-100), in the page's "ring" group.
nlohmann::json ring_fields(const RingStateSpec& state, Rgb default_color, int default_brightness);

}  // namespace ld
