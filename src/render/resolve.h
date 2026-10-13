// src/render/resolve.h
#pragma once
#include <cstdint>
#include "render/look.h"

namespace ld {

// sRGB code (0-255) <-> linear light (0-1), per channel.
double srgb_to_linear(std::uint8_t v);
std::uint8_t linear_to_srgb(double v);

// The colour of `value` on a three-stop scale: interpolated in linear light between the
// two stops around it, clamped to the end colours. Stops are taken in the order of `at`; NaN is cold.
Rgb gradient_color(const Gradient& g, double value);

// One element's effective style: the module's defaults (spec, then
// `in.size` / `in.color`), then its text role (`role`, may be null: the size kept within the
// element's bounds, the weight, the font), then the user's settings for the element (`over`, may be
// null), then the state. Colour priority: idle / stale > gradient > warning / critical > fixed colour.
// Warning and critical need the State or Gradient mode; labels and units (keeps_color) follow the
// state only when the user chose "state" or "gradient". Icons take no weight; icons and graphs no role.
ResolvedStyle resolve(const ElementSpec& spec, const ElementOverride* over, const ElementInput& in,
                      const StatePalette& palette, const RoleLook* role = nullptr);

}  // namespace ld
