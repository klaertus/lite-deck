#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include "core/winrt.h"
#include "modules/common/marquee.h"

using namespace ld;

static int lit_columns(Surface& s, int y0, int y1) {
  int cols = 0;
  for (int x = 0; x < s.width(); ++x) {
    for (int y = y0; y < y1; y += 2) {
      auto p = s.pixel(x, y);
      if (p.r + p.g + p.b > 200) {
        ++cols;
        break;
      }
    }
  }
  return cols;
}

TEST_CASE("draw_marquee draws short text once and reports no overflow") {
  Renderer r;
  Surface s(r, 300, 40);
  TextStyle st;
  st.size = 20;
  st.align = HAlign::Left;
  bool over;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    over = draw_marquee(c, L"Court", RectF{0, 0, 300, 40}, st, 0);
  }
  CHECK_FALSE(over);
  CHECK(lit_columns(s, 0, 40) > 10);
}

TEST_CASE("draw_marquee scrolls long text and wraps seamlessly") {
  Renderer r;
  Surface s(r, 120, 40);
  TextStyle st;
  st.size = 20;
  st.align = HAlign::Left;
  std::wstring text = L"A title far too long for the zone";
  bool over;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    over = draw_marquee(c, text, RectF{0, 0, 120, 40}, st, 0);
  }
  CHECK(over);
  float loop = 0;
  {
    Canvas c = s.canvas();
    loop = c.measure(text, st) + marquee_gap(st);
  }
  // offset == loop draws exactly like offset == 0
  std::vector<int> a, b;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    draw_marquee(c, text, RectF{0, 0, 120, 40}, st, 0);
  }
  for (int x = 0; x < 120; x += 3) a.push_back(s.pixel(x, 20).r);
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    draw_marquee(c, text, RectF{0, 0, 120, 40}, st, loop);
  }
  for (int x = 0; x < 120; x += 3) b.push_back(s.pixel(x, 20).r);
  CHECK(a == b);
}

TEST_CASE("draw_marquee stays inside its area") {
  Renderer r;
  Surface s(r, 200, 40);
  TextStyle st;
  st.size = 20;
  st.align = HAlign::Left;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    draw_marquee(c, L"A title far too long for the zone", RectF{50, 0, 100, 40}, st, 0, MarqueeOptions{Rgb{0, 0, 0}});
  }
  for (int x = 0; x < 50; ++x) CHECK(s.pixel(x, 20).r == 0);
  for (int x = 150; x < 200; ++x) CHECK(s.pixel(x, 20).r == 0);
}

// Brightest pixel of a column, over the area's rows.
static int column_max(Surface& s, int x, int y0, int y1) {
  int m = 0;
  for (int y = y0; y < y1; ++y) {
    auto p = s.pixel(x, y);
    m = std::max(m, p.r + p.g + p.b);
  }
  return m;
}

TEST_CASE("draw_marquee fades the edges of overflowing text into the background") {
  Renderer r;
  Surface s(r, 300, 40);
  TextStyle st;
  st.size = 24;
  st.bold = true;
  st.align = HAlign::Left;
  // A solid run of glyphs so every part of the box carries ink.
  const std::wstring text = L"MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
  const RectF area{20, 0, 260, 40};
  // At rest, scrolled a little, scrolled past the fade, and many loops later (wrapped offset).
  for (float off : {0.f, 10.f, 37.f, 5003.f}) {
    {
      Canvas c = s.canvas();
      c.clear({0, 0, 0});
      CHECK(draw_marquee(c, text, area, st, off, MarqueeOptions{Rgb{0, 0, 0}}));
    }
    CAPTURE(off);
    // Brightest column in a window of a few (a single one may fall between two glyphs).
    auto win = [&](int x0) {
      int m = 0;
      for (int x = x0; x < x0 + 5; ++x) m = std::max(m, column_max(s, x, 0, 40));
      return m;
    };
    int mid = 0;
    for (int x = 130; x < 170; ++x) mid = std::max(mid, column_max(s, x, 0, 40));
    REQUIRE(mid > 600);
    // Right edge: always faded, nearly background at the box edge, rising inward.
    CHECK(column_max(s, 279, 0, 40) < mid / 8);
    CHECK(win(274) < win(259));
    CHECK(win(259) < mid);
    if (off >= kMarqueeFade) {  // scrolled: the left edge fades the same way
      CHECK(column_max(s, 20, 0, 40) < mid / 8);
      CHECK(win(21) < win(36));
      CHECK(win(36) < mid);
    } else if (off == 0) {  // at rest: the first letter stays crisp
      CHECK(win(20) == mid);
    }
  }
}

TEST_CASE("draw_marquee leaves text that fits untouched by the fade") {
  Renderer r;
  Surface a(r, 300, 40), b(r, 300, 40);
  TextStyle st;
  st.size = 20;
  st.align = HAlign::Left;
  {
    Canvas c = a.canvas();
    c.clear({0, 0, 0});
    CHECK_FALSE(draw_marquee(c, L"Court", RectF{0, 0, 300, 40}, st, 25, MarqueeOptions{Rgb{0, 0, 0}}));
  }
  {
    Canvas c = b.canvas();
    c.clear({0, 0, 0});
    ClipGuard clip(c, RectF{0, 0, 300, 40});
    c.text(L"Court", RectF{0, 0, 300, 40}, st);
  }
  CHECK(a.pixels() == b.pixels());
}

TEST_CASE("marquee fade width is clamped on narrow boxes") {
  CHECK(marquee_fade_width(300) == doctest::Approx(kMarqueeFade));
  CHECK(marquee_fade_width(60) == doctest::Approx(20.0));
  CHECK(marquee_fade_width(0) == doctest::Approx(0.0));
}

TEST_CASE("scroll_offset matches the plugin speed scale") {
  CHECK(scroll_offset(Duration(1000), 2.0) == doctest::Approx(60.0));
  CHECK(marquee_gap(TextStyle{L"Segoe UI", 10}) == doctest::Approx(40.0));
  CHECK(marquee_gap(TextStyle{L"Segoe UI", 30}) == doctest::Approx(60.0));
}

TEST_CASE("the marquee scrolls as smoothly after days as in the first second") {
  // Speed 2 (60 px/s), 33 ms frames: each frame moves the same 1.98 px, after one second or six days.
  const Duration frame(33);
  for (Duration start : {Duration(1000), Duration(6LL * 24 * 3600 * 1000)}) {
    CAPTURE(start.count());
    for (int k = 0; k < 4; ++k) {
      const double step = scroll_offset(start + frame * (k + 1), 2.0) - scroll_offset(start + frame * k, 2.0);
      CHECK(step == doctest::Approx(1.98).epsilon(1e-6));
    }
  }
  // And it draws exactly as the same phase of the first loop: no whole-pixel jumps from float rounding.
  Renderer r;
  Surface s(r, 120, 40);
  TextStyle st;
  st.size = 20;
  st.align = HAlign::Left;
  const std::wstring text = L"A title far too long for the zone";
  float loop = 0;
  {
    Canvas c = s.canvas();
    loop = c.measure(text, st) + marquee_gap(st);
  }
  auto draw_at = [&](double off) {
    {
      Canvas c = s.canvas();
      c.clear({0, 0, 0});
      draw_marquee(c, text, RectF{0, 0, 120, 40}, st, off);
    }
    return s.pixels();
  };
  for (int k = 0; k < 4; ++k) {
    const double off = scroll_offset(Duration(6LL * 24 * 3600 * 1000) + frame * k, 2.0);  // ~31 M px
    CAPTURE(off);
    CHECK(draw_at(off) == draw_at(std::fmod(off, static_cast<double>(loop))));
  }
}

TEST_CASE("C++/WinRT is usable") {
  winrt::Windows::Foundation::Uri u(L"https://example.com/a");
  CHECK(to_wide(u.Host()) == L"example.com");
}
