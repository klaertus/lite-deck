// src/modules/sun/sun_calc.h
#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ld {

struct CivilDate {
  int year = 1970, month = 1, day = 1;
  bool operator==(const CivilDate&) const = default;
};
// Days since 1970-01-01 (proleptic Gregorian). `day` may overflow: {2026, 1, 32} is February 1st.
std::int64_t days_from_civil(CivilDate date);
CivilDate civil_from_days(std::int64_t days);
std::optional<CivilDate> parse_iso_date(std::string_view text);  // "2026-10-11"
std::string format_iso_date(CivilDate date);                      // "2026-10-11"

enum class SunKind { Normal, PolarDay, PolarNight };
struct SunTimes {
  SunKind kind = SunKind::Normal;
  std::int64_t rise = 0;  // unix seconds (UTC); meaningful when kind == Normal
  std::int64_t set = 0;
};

// NOAA solar calculator (Meeus): sunrise and sunset of the civil `date` at `lat`/`lon` (degrees,
// north and east positive), for the standard altitude of -0.833° (refraction and solar radius).
// Each event is computed twice, the second time with the sun's position at the first estimate.
SunTimes sun_times(CivilDate date, double lat, double lon);

// The time zone the key shows times in: the PC's, not the city's. Tests inject a fake one.
struct LocalZone {
  std::function<std::tm(std::int64_t unix_s)> to_local;  // like localtime_s
  std::function<std::int64_t(CivilDate date)> midnight;   // unix seconds of 00:00 local on `date`
};

// Online minus local, in seconds, applied to the local times until the next online check.
struct SunCorrection {
  std::int64_t rise_s = 0, set_s = 0;
  bool operator==(const SunCorrection&) const = default;
};

enum class SunNext { Rise, Set };
struct SunView {
  SunKind kind = SunKind::Normal;
  std::int64_t rise = 0, set = 0;  // unix seconds, corrected; meaningful when kind == Normal
  SunNext next = SunNext::Rise;    // the event to highlight
  std::int64_t change_at = 0;      // unix seconds: the next event or the next local midnight
};
// What the key shows at `now`: today's sunrise and sunset, or tomorrow's once today's sunset has
// passed; the next event is highlighted. A polar day or night changes only at midnight.
SunView sun_view(std::int64_t now, double lat, double lon, const LocalZone& zone, const SunCorrection& correction);

// "07:52": local time rounded to the nearest minute.
std::wstring format_hm(std::int64_t unix_s, const LocalZone& zone);

// Open-Meteo's daily sunrise and sunset (forecast API, timezone=auto), converted to unix seconds
// with utc_offset_seconds. `date` is the city's local date of the values.
struct OnlineSun {
  CivilDate date;
  std::int64_t rise = 0, set = 0;
};
std::optional<OnlineSun> parse_online_sun(std::string_view body);
std::string sun_check_url(double lat, double lon);

inline constexpr std::int64_t kSunMaxGap = 1200;  // seconds: a larger gap cannot come from the algorithm (about 75 s)
inline constexpr std::int64_t kSunTolerance = 120;  // seconds: |online - local| above this is a mismatch
struct SunCheck {
  // usable == false: the answer cannot be trusted (local is a polar day or night, or a gap above
  // kSunMaxGap, typically a wrong utc_offset_seconds on a DST-switch day). Then agree is false and
  // the correction is zero: the caller logs it and keeps the local times, with no correction.
  // usable == true: agree tells whether both events are within kSunTolerance; if not, apply `correction`.
  bool usable = true;
  bool agree = true;
  SunCorrection correction;  // online - local; zero when !usable
};
SunCheck compare_sun(const SunTimes& local, const OnlineSun& online);

}  // namespace ld
