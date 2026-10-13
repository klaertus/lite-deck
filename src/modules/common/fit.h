// src/modules/common/fit.h
#pragma once
#include <string_view>
#include "render/renderer.h"

namespace ld {
// Reduces style.size 1 px at a time while `text` is wider than `max_width`, never below `min_size`
// (a text never overflows its place). Same result as that loop, in at most ~7 measures,
// except that a fractional start stops at `min_size` instead of the step below it.
void shrink_to_fit(Canvas& c, std::wstring_view text, TextStyle& style, float max_width, int min_size);
// shrink_to_fit to rect.w, then draws `text` in `rect`.
void fit_text(Canvas& c, std::wstring_view text, RectF rect, TextStyle style, int min_size);
}  // namespace ld
