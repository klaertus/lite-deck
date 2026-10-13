// src/modules/common/temp_header.h
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "render/renderer.h"

namespace ld {

// The small temperature at the top right of a key (cpu_load, gpu usage), resizable on the page. It
// follows the thresholds (State); each module gives it its hue with tinted().
inline constexpr ElementSpec kTempElement{.key = "temp", .label = "Temperature", .size = 17, .weight = 600,
                                          .min_size = 8, .max_size = 30, .row = Row::Top, .align = HAlign::Right,
                                          .color_mode = ColorMode::State, .color = Rgb{255, 255, 255},
                                          .gradient = true, .role = TextRole::Secondary};

// "52 °C" (narrow no-break space), or a dash when there is no fresh value.
std::wstring format_temperature(std::optional<int> celsius);

}  // namespace ld
