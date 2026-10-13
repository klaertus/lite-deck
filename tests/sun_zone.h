// tests/sun_zone.h: a fixed Europe/Brussels zone for 2025-2027, whatever the time zone of the PC
// running the tests. CET (+1 h); CEST (+2 h) from the last Sunday of March to the last Sunday of
// October, switching at 01:00 UTC.
#pragma once
#include <cstdint>
#include <ctime>
#include "modules/sun/sun_calc.h"

inline std::int64_t brussels_offset(std::int64_t unix_s) {
  auto utc = [](int y, int m, int d) { return ld::days_from_civil({y, m, d}) * 86400 + 3600; };
  const std::int64_t summers[][2] = {
      {utc(2025, 3, 30), utc(2025, 10, 26)},
      {utc(2026, 3, 29), utc(2026, 10, 25)},
      {utc(2027, 3, 28), utc(2027, 10, 31)},
  };
  for (const auto& s : summers)
    if (unix_s >= s[0] && unix_s < s[1]) return 7200;
  return 3600;
}

inline ld::LocalZone brussels_zone() {
  ld::LocalZone z;
  z.to_local = [](std::int64_t s) {
    const std::int64_t local = s + brussels_offset(s);
    const std::int64_t day = local / 86400;  // tests stay after 1970: no negative division
    const ld::CivilDate d = ld::civil_from_days(day);
    const std::int64_t sec = local - day * 86400;
    std::tm t{};
    t.tm_year = d.year - 1900;
    t.tm_mon = d.month - 1;
    t.tm_mday = d.day;
    t.tm_hour = static_cast<int>(sec / 3600);
    t.tm_min = static_cast<int>(sec % 3600 / 60);
    t.tm_sec = static_cast<int>(sec % 60);
    return t;
  };
  z.midnight = [](ld::CivilDate d) {
    const std::int64_t naive = ld::days_from_civil(d) * 86400;
    return naive - brussels_offset(naive - 7200);  // local midnight is never inside a switch hour
  };
  return z;
}
