// src/modules/common/key_look.h: what the key modules share in the design system look.
#pragma once
#include "render/look.h"

namespace ld {

// Labels, units and secondary lines: one grey.
inline constexpr Rgb kSecondaryGrey{0x8c, 0x8c, 0x8c};

// `spec` in another normal colour (a shared element, such as the temperature, in a module's hue).
constexpr ElementSpec tinted(ElementSpec spec, Rgb color) {
  spec.color = color;
  return spec;
}

// The "seconds below" layout of the clock and the stopwatch: hours:minutes as a main value in the
// middle row, the seconds smaller in the bottom row. Both are their own elements, so the user sets
// their colour, size and role apart from the one-line "time".
// The seconds' default look: A, the colour of the time; B, the secondary grey. Chosen here for both modules.
enum class SecondsStyle { SameColor, Grey };
inline constexpr SecondsStyle kSecondsStyle = SecondsStyle::Grey;

// Hours:minutes, from the module's one-line `time`: the size and role of a main value.
constexpr ElementSpec hours_minutes_spec(ElementSpec time) {
  time.key = "hours_minutes";
  time.label = "Hours:minutes (seconds below)";
  time.size = 36;
  time.max_size = 80;
  time.row = Row::Middle;
  time.role = TextRole::Value;
  return time;
}

// The seconds under hours:minutes, centred in the bottom row, in `style`.
constexpr ElementSpec seconds_spec(ElementSpec time, SecondsStyle style = kSecondsStyle) {
  ElementSpec s = time;  // A: the time's colour and colour mode
  s.key = "seconds";
  s.label = "Seconds (below)";
  s.size = 20;
  s.weight = 700;
  s.min_size = 8;
  s.max_size = 40;
  s.row = Row::Bottom;
  s.align = HAlign::Center;
  s.role = TextRole::Secondary;
  if (style == SecondsStyle::Grey) {  // B: like the other secondary texts
    s.weight = 600;
    s.color_mode = ColorMode::Fixed;
    s.color = kSecondaryGrey;
    s.keeps_color = true;
    s.gradient = false;
  }
  return s;
}

}  // namespace ld
