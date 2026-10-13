#include "modules/common/marquee.h"
#include <algorithm>
#include <cmath>

namespace ld {

float marquee_gap(const TextStyle& style) { return std::max(style.size * 2.f, 40.f); }

double scroll_offset(Duration elapsed, double scroll_speed) {
  return static_cast<double>(elapsed.count()) * scroll_speed * 30.0 / 1000.0;
}

float marquee_fade_width(float area_w) { return std::clamp(area_w / 3.f, 0.f, kMarqueeFade); }

bool draw_marquee(Canvas& c, std::wstring_view text, RectF area, const TextStyle& style, double offset,
                  const MarqueeOptions& opt) {
  float w = c.measure(text, style);
  ClipGuard clip(c, area);
  if (w <= area.w) {
    c.text(text, area, style);
    return false;
  }
  TextStyle left = style;
  left.align = HAlign::Left;
  float loop = w + marquee_gap(style);
  // Wrapped to one loop in double, then a float: as smooth after days as in the first second.
  const double unwrapped = std::max(offset, 0.0);
  const float o = loop > 0 ? static_cast<float>(std::fmod(unwrapped, static_cast<double>(loop))) : 0.f;
  c.text(text, RectF{area.x - o, area.y, w + 2, area.h}, left);
  // The second copy only when it reaches the box (a size of margin for overhanging glyphs).
  if (loop - o < area.w + style.size) c.text(text, RectF{area.x - o + loop, area.y, w + 2, area.h}, left);
  if (opt.fade) {
    // Smooth fades into the background: opaque at the box edge, clear inward. At rest the text
    // starts flush left and its first letters stay crisp; the left fade grows in with the first
    // pixels of scrolling (the unwrapped offset, so it never pops off at a loop's wrap).
    const float f = marquee_fade_width(area.w);
    const float fl = static_cast<float>(std::min(static_cast<double>(f), unwrapped));
    if (fl > 0) c.fade_rect(RectF{area.x, area.y, fl, area.h}, *opt.fade, true);
    c.fade_rect(RectF{area.x + area.w - f, area.y, f, area.h}, *opt.fade, false);
  }
  return true;
}

}  // namespace ld
