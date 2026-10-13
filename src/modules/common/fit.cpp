// src/modules/common/fit.cpp
#include "modules/common/fit.h"
#include <algorithm>
#include <cmath>

namespace ld {

// The rule is "size -= 1 while size > min and too wide": the result is start - k for the smallest k
// that fits, or the first one that reaches the minimum. The width grows with the size, so k is found
// by bisection, which keeps the text format cache from filling with one format per tried size.
// Subtracting a whole number from these floats is exact: start - k equals k subtractions of 1.
// From a fractional start (32.5), the last step would land below the minimum (9.5): it stops at it.
void shrink_to_fit(Canvas& c, std::wstring_view text, TextStyle& style, float max_width, int min_size) {
  const float start = style.size;
  const float floor = static_cast<float>(min_size);
  if (start <= floor || c.measure(text, style) <= max_width) return;
  int lo = 1;
  int hi = static_cast<int>(std::ceil(start - floor));  // start - hi <= floor: where the loop stops anyway
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    style.size = start - static_cast<float>(mid);
    if (c.measure(text, style) <= max_width) hi = mid;
    else lo = mid + 1;
  }
  style.size = std::max(start - static_cast<float>(lo), floor);
}

void fit_text(Canvas& c, std::wstring_view text, RectF rect, TextStyle style, int min_size) {
  shrink_to_fit(c, text, style, rect.w, min_size);
  c.text(text, rect, style);
}

}  // namespace ld
