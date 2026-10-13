#include <doctest/doctest.h>
#include <chrono>
#include <string>
#include "modules/time_zone.h"

using namespace std::chrono_literals;

TEST_CASE("TimeZoneWatch reloads the zone when it changed, checking at most once a minute") {
  std::wstring zone = L"Romance Standard Time|-60";
  int reloads = 0, reads = 0;
  ld::TimeZoneWatch w(
      [&] {
        ++reads;
        return zone;
      },
      [&] { ++reloads; });
  const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
  CHECK_FALSE(w.check(t0));  // the first check records the zone
  CHECK(reloads == 0);
  zone = L"Pacific Standard Time|480";
  CHECK_FALSE(w.check(t0 + 59s));  // too soon: Windows is not even asked
  CHECK(reads == 1);
  CHECK(w.check(t0 + 60s));
  CHECK(reloads == 1);
  CHECK_FALSE(w.check(t0 + 120s));  // unchanged since
  CHECK(reloads == 1);
}

TEST_CASE("refresh_time_zone is cheap to call often and never throws") {
  CHECK_NOTHROW(ld::refresh_time_zone());
  CHECK_FALSE(ld::refresh_time_zone());  // within the minute: no check at all
}
