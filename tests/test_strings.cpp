#include <doctest/doctest.h>
#include "core/strings.h"

TEST_CASE("widen and narrow round-trip UTF-8") {
  const std::string s = "Temperature 42 °C - ok";
  CHECK(ld::narrow(ld::widen(s)) == s);
  CHECK(ld::widen("é") == L"é");
  CHECK(ld::widen("").empty());
}

TEST_CASE("widen and narrow replace invalid sequences instead of failing") {
  CHECK(ld::widen("\xff") == L"\xFFFD");
  CHECK(ld::widen("ok\xC3") == L"ok\xFFFD");                         // truncated sequence at the end
  CHECK(ld::narrow(std::wstring(1, L'\xD800')) == "\xEF\xBF\xBD");    // lone surrogate
  CHECK(ld::narrow(L"Liège") == "Liège");
}
