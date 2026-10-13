// src/modules/sun/sun_calc.cpp
#include "modules/sun/sun_calc.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>
#include <nlohmann/json.hpp>

namespace ld {

std::int64_t days_from_civil(CivilDate date) {  // H. Hinnant's algorithm
  std::int64_t y = date.year;
  const std::int64_t m = date.month;
  if (m <= 2) --y;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + date.day - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

CivilDate civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return CivilDate{static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0)), month, day};
}

std::optional<CivilDate> parse_iso_date(std::string_view text) {
  int y = 0, m = 0, d = 0;
  char tail = 0;
  const std::string s(text);
  if (sscanf_s(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &tail, 1u) != 3) return std::nullopt;
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
  return CivilDate{y, m, d};
}

std::string format_iso_date(CivilDate d) { return std::format("{:04}-{:02}-{:02}", d.year, d.month, d.day); }

namespace {

constexpr double kRad = std::numbers::pi / 180.0;

struct SunPosition {
  double declination;  // radians
  double eq_time;      // minutes
};

// The NOAA spreadsheet formulas at Julian day `jd`.
SunPosition sun_position(double jd) {
  const double t = (jd - 2451545.0) / 36525.0;  // Julian centuries since J2000
  double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
  if (l0 < 0) l0 += 360.0;
  const double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
  const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const double c = std::sin(m * kRad) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                   std::sin(2 * m * kRad) * (0.019993 - 0.000101 * t) + std::sin(3 * m * kRad) * 0.000289;
  const double omega = 125.04 - 1934.136 * t;
  const double lambda = l0 + c - 0.00569 - 0.00478 * std::sin(omega * kRad);  // apparent longitude
  const double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
  const double eps = eps0 + 0.00256 * std::cos(omega * kRad);  // corrected obliquity
  const double y = std::pow(std::tan(eps * kRad / 2), 2);
  const double eq = 4.0 / kRad *
                    (y * std::sin(2 * l0 * kRad) - 2 * e * std::sin(m * kRad) +
                     4 * e * y * std::sin(m * kRad) * std::cos(2 * l0 * kRad) - 0.5 * y * y * std::sin(4 * l0 * kRad) -
                     1.25 * e * e * std::sin(2 * m * kRad));
  return {std::asin(std::sin(eps * kRad) * std::sin(lambda * kRad)), eq};
}

struct Event {
  SunKind kind;
  double minutes;  // after 00:00 UTC of the date; may be < 0 or >= 1440
};

Event event_minutes(std::int64_t day, double lat, double lon, double sign) {  // sign: -1 rise, +1 set
  const double jd0 = static_cast<double>(day) + 2440587.5;
  double minutes = 720.0 - 4.0 * lon;  // first estimate: solar noon
  for (int pass = 0; pass < 2; ++pass) {
    const SunPosition p = sun_position(jd0 + minutes / 1440.0);
    const double cos_ha = std::cos(90.833 * kRad) / (std::cos(lat * kRad) * std::cos(p.declination)) -
                          std::tan(lat * kRad) * std::tan(p.declination);
    if (cos_ha > 1.0) return {SunKind::PolarNight, 0};
    if (cos_ha < -1.0) return {SunKind::PolarDay, 0};
    minutes = 720.0 - 4.0 * lon - p.eq_time + sign * 4.0 * std::acos(cos_ha) / kRad;
  }
  return {SunKind::Normal, minutes};
}

}  // namespace

SunTimes sun_times(CivilDate date, double lat, double lon) {
  lat = std::clamp(lat, -89.9, 89.9);  // the poles: no division by zero
  const std::int64_t day = days_from_civil(date);
  const Event rise = event_minutes(day, lat, lon, -1.0);
  const Event set = event_minutes(day, lat, lon, 1.0);
  if (rise.kind != SunKind::Normal) return {rise.kind, 0, 0};
  if (set.kind != SunKind::Normal) return {set.kind, 0, 0};
  return {SunKind::Normal, day * 86400 + std::llround(rise.minutes * 60.0), day * 86400 + std::llround(set.minutes * 60.0)};
}

SunView sun_view(std::int64_t now, double lat, double lon, const LocalZone& zone, const SunCorrection& corr) {
  const std::tm t = zone.to_local(now);
  const std::int64_t today = days_from_civil({t.tm_year + 1900, t.tm_mon + 1, t.tm_mday});
  std::int64_t next_midnight = zone.midnight(civil_from_days(today + 1));
  if (next_midnight <= now) next_midnight = now + 3600;  // mktime failed (-1): an hour, not a wake-up every second
  auto of_day = [&](std::int64_t day) {
    const SunTimes s = sun_times(civil_from_days(day), lat, lon);
    SunView v;
    v.kind = s.kind;
    v.rise = s.rise + corr.rise_s;
    v.set = s.set + corr.set_s;
    return v;
  };
  SunView v = of_day(today);
  if (v.kind == SunKind::Normal && now >= v.set) v = of_day(today + 1);
  if (v.kind != SunKind::Normal) {
    v.change_at = next_midnight;
    return v;
  }
  v.next = now < v.rise ? SunNext::Rise : SunNext::Set;
  v.change_at = std::min(v.next == SunNext::Rise ? v.rise : v.set, next_midnight);
  return v;
}

std::wstring format_hm(std::int64_t unix_s, const LocalZone& zone) {
  const std::tm t = zone.to_local((unix_s + 30) / 60 * 60);  // nearest minute (unix_s > 0)
  return std::format(L"{:02}:{:02}", t.tm_hour, t.tm_min);
}

namespace {
// "2026-10-11T07:57" read as if it were UTC.
std::optional<std::int64_t> parse_iso_minute(const std::string& s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  if (sscanf_s(s.c_str(), "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) != 5) return std::nullopt;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 24 || mi < 0 || mi > 59) return std::nullopt;
  return days_from_civil({y, mo, d}) * 86400 + h * 3600LL + mi * 60LL;
}
}  // namespace

std::optional<OnlineSun> parse_online_sun(std::string_view body) {
  using nlohmann::json;
  const json j = json::parse(body, nullptr, false);
  try {
    if (!j.is_object()) return std::nullopt;
    const std::int64_t offset = j.at("utc_offset_seconds").get<std::int64_t>();
    const json& daily = j.at("daily");
    const auto date = parse_iso_date(daily.at("time").at(0).get<std::string>());
    const auto rise = parse_iso_minute(daily.at("sunrise").at(0).get<std::string>());
    const auto set = parse_iso_minute(daily.at("sunset").at(0).get<std::string>());
    if (!date || !rise || !set) return std::nullopt;
    return OnlineSun{*date, *rise - offset, *set - offset};
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

std::string sun_check_url(double lat, double lon) {
  return std::format(
      "https://api.open-meteo.com/v1/forecast?latitude={:.4f}&longitude={:.4f}"
      "&daily=sunrise,sunset&timezone=auto&forecast_days=1",
      lat, lon);
}

SunCheck compare_sun(const SunTimes& local, const OnlineSun& online) {
  SunCheck c;
  if (local.kind != SunKind::Normal) {
    c.usable = false;
    c.agree = false;
    return c;
  }
  const SunCorrection diff{online.rise - local.rise, online.set - local.set};
  if (std::llabs(diff.rise_s) > kSunMaxGap || std::llabs(diff.set_s) > kSunMaxGap) {
    c.usable = false;
    c.agree = false;
    return c;
  }
  c.correction = diff;
  c.agree = std::llabs(diff.rise_s) <= kSunTolerance && std::llabs(diff.set_s) <= kSunTolerance;
  return c;
}

}  // namespace ld
