#include <doctest/doctest.h>
#include "modules/common/fit.h"

using namespace ld;

TEST_CASE("shrink_to_fit gives the same size as shrinking 1 px at a time") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  struct Case {
    const wchar_t* text;
    float size;
    float width;
    int min;
  };
  const Case cases[] = {
      {L"13:37:42", 60, 112, 10},  {L"13:37:42", 27, 112, 10},       {L"Wednesday", 40, 108, 9},
      {L"₿ 98 512 €", 40, 104, 9}, {L"A very long text that never fits", 60, 20, 12},
      {L"Gold 2 432 €", 25.76f, 104, 9},
  };
  for (const Case& k : cases) {
    TextStyle fast;
    fast.size = k.size;
    fast.weight = 700;
    TextStyle slow = fast;
    shrink_to_fit(c, k.text, fast, k.width, k.min);
    while (slow.size > static_cast<float>(k.min) && c.measure(k.text, slow) > k.width) slow.size -= 1;
    CHECK(fast.size == slow.size);
  }
}

TEST_CASE("shrink_to_fit keeps a text that fits and stops at the minimum") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  TextStyle fits;
  fits.size = 20;
  shrink_to_fit(c, L"12", fits, 112, 8);
  CHECK(fits.size == 20.f);
  TextStyle never;
  never.size = 60;
  shrink_to_fit(c, L"A very long text that never fits", never, 20, 12);
  CHECK(never.size == 12.f);
  for (float start : {32.5f, 21.28f}) {  // fractional starts: the last 1 px step would pass the minimum
    TextStyle frac;
    frac.size = start;
    shrink_to_fit(c, L"A very long text that never fits", frac, 20, 10);
    CHECK(frac.size == 10.f);
  }
  TextStyle big;
  big.size = 60;
  big.weight = 700;
  shrink_to_fit(c, L"13:37:42", big, 112, 10);
  CHECK(c.measure(L"13:37:42", big) <= 112.f);
  CHECK(big.size < 60.f);
}
