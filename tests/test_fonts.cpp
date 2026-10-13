#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include "render/fonts.h"

TEST_CASE("system_font_families lists Segoe UI once, sorted, without duplicates, built once") {
  const auto t0 = std::chrono::steady_clock::now();
  const auto& fonts = ld::system_font_families();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE(fonts.size() << " font families listed in " << ms << " ms");
  REQUIRE(fonts.size() > 10);
  CHECK(std::count(fonts.begin(), fonts.end(), "Segoe UI") == 1);
  auto less = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; };
  auto same = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; };
  CHECK(std::is_sorted(fonts.begin(), fonts.end(), less));
  CHECK(std::adjacent_find(fonts.begin(), fonts.end(), same) == fonts.end());
  CHECK(std::none_of(fonts.begin(), fonts.end(), [](const std::string& f) { return f.empty(); }));
  CHECK(&ld::system_font_families() == &fonts);  // cached: the same vector
  CHECK(ms < 2000);
}

TEST_CASE("font_installed knows installed families, whatever the case, and nothing else") {
  CHECK(ld::font_installed(L"Segoe UI"));
  CHECK(ld::font_installed(L"segoe ui"));
  CHECK(ld::font_installed(L"Consolas"));
  CHECK_FALSE(ld::font_installed(L"No Such Font 4242"));
  CHECK_FALSE(ld::font_installed(L""));
}
