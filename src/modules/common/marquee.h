#pragma once
#include <optional>
#include <string_view>
#include "core/types.h"
#include "render/renderer.h"

namespace ld {
struct MarqueeOptions {
  std::optional<Rgb> fade;  // background colour: fades the clipped edges into it
};
inline constexpr float kMarqueeFade = 28;  // px of edge fade on overflowing text, each side
float marquee_fade_width(float area_w);                       // kMarqueeFade, at most a third of the box
float marquee_gap(const TextStyle& style);                     // max(2 * size, 40)
inline constexpr int kFastLoops = 5;  // a long text scrolls smoothly this many passes, then at 20 fps to save CPU
// True once an overflowing text (`text_w` wide plus the gap) has been scrolled past kFastLoops times.
inline bool scroll_is_slow(double offset, float text_w, float gap) {
  return offset >= static_cast<double>(text_w + gap) * kFastLoops;
}
// px; speed = px per frame at 30 fps. Unwrapped, so kept in double: a text that stays on for days
// reaches millions of px, where a float has whole-pixel steps (draw_marquee wraps it in double).
double scroll_offset(Duration elapsed, double scroll_speed);
// Draws `text` clipped to `area`. If it fits, draws it once (aligned per `style`) and returns false.
// Otherwise draws two left-aligned copies scrolled by `offset` (modulo text width + gap) and returns
// true (the text scrolls endlessly).
bool draw_marquee(Canvas& c, std::wstring_view text, RectF area, const TextStyle& style, double offset,
                  const MarqueeOptions& opt = {});
}  // namespace ld
