#include <doctest/doctest.h>
#include <cstdlib>
#include "modules/sun/sun_calc.h"
#include "sun_zone.h"

using ld::CivilDate;
using ld::SunKind;

namespace {
// Open-Meteo's values (UTC, truncated to the minute, computed on its grid a few km away).
struct Ref {
  CivilDate date;
  int rise_h, rise_m, set_h, set_m;
};
std::int64_t at(CivilDate d, int h, int m) { return ld::days_from_civil(d) * 86400 + h * 3600LL + m * 60LL; }
void check_near(std::int64_t actual, std::int64_t expected) {
  INFO("actual " << actual << ", expected " << expected << ", diff " << (actual - expected) << " s");
  CHECK(std::llabs(actual - expected) <= 120);
}
constexpr double kBxlLat = 50.8503, kBxlLon = 4.3517;  // Brussels
}  // namespace

TEST_CASE("days_from_civil and civil_from_days round trip, with day overflow") {
  CHECK(ld::days_from_civil({1970, 1, 1}) == 0);
  CHECK(ld::days_from_civil({2000, 1, 1}) == 10957);
  CHECK(ld::days_from_civil({2026, 10, 11}) == 20737);
  CHECK(ld::days_from_civil({2026, 1, 32}) == ld::days_from_civil({2026, 2, 1}));
  CHECK(ld::civil_from_days(ld::days_from_civil({2026, 12, 32})) == CivilDate{2027, 1, 1});
  CHECK(ld::civil_from_days(ld::days_from_civil({2024, 2, 29})) == CivilDate{2024, 2, 29});
  CHECK(ld::civil_from_days(20737) == CivilDate{2026, 10, 11});
}

TEST_CASE("iso dates parse and format") {
  CHECK(ld::parse_iso_date("2026-10-11") == CivilDate{2026, 10, 11});
  CHECK_FALSE(ld::parse_iso_date("2026-13-01"));
  CHECK_FALSE(ld::parse_iso_date("yesterday"));
  CHECK_FALSE(ld::parse_iso_date("2026-10-11x"));
  CHECK(ld::format_iso_date({2026, 3, 9}) == "2026-03-09");
}

TEST_CASE("sun_times matches Open-Meteo in Brussels within 2 min, all seasons and both DST changes") {
  const Ref refs[] = {
      {{2026, 3, 28}, 5, 27, 18, 8},   // last day of winter time
      {{2026, 3, 29}, 5, 25, 18, 10},  // spring forward
      {{2026, 6, 21}, 3, 28, 20, 0},   // longest day
      {{2025, 12, 21}, 7, 42, 15, 38}, // shortest day
      {{2026, 10, 24}, 6, 20, 16, 32}, // last day of summer time
      {{2026, 10, 25}, 6, 22, 16, 30}, // fall back
  };
  for (const Ref& r : refs) {
    INFO(ld::format_iso_date(r.date));
    const ld::SunTimes s = ld::sun_times(r.date, kBxlLat, kBxlLon);
    REQUIRE(s.kind == SunKind::Normal);
    check_near(s.rise, at(r.date, r.rise_h, r.rise_m));
    check_near(s.set, at(r.date, r.set_h, r.set_m));
  }
}

TEST_CASE("sun_times matches Open-Meteo in Brussels on 2026-10-11") {
  const ld::SunTimes s = ld::sun_times({2026, 10, 11}, 50.8503, 4.3517);
  REQUIRE(s.kind == SunKind::Normal);
  check_near(s.rise, at({2026, 10, 11}, 5, 58));
  check_near(s.set, at({2026, 10, 11}, 16, 58));
}

TEST_CASE("sun_view never schedules the next change in the past when the zone cannot compute a midnight") {
  ld::LocalZone z = brussels_zone();
  z.midnight = [](CivilDate) -> std::int64_t { return -1; };  // what a failed mktime returns
  const std::int64_t now = at({2026, 6, 21}, 10, 0);
  const ld::SunView polar = ld::sun_view(now, 69.6496, 18.9560, z, {});
  CHECK(polar.kind == SunKind::PolarDay);
  CHECK(polar.change_at == now + 3600);
  const ld::SunView normal = ld::sun_view(at({2026, 6, 21}, 21, 0), kBxlLat, kBxlLon, z, {});  // after sunset
  CHECK(normal.change_at > at({2026, 6, 21}, 21, 0));
}

TEST_CASE("sun_times reports the polar day and the polar night in Tromso") {
  CHECK(ld::sun_times({2026, 6, 21}, 69.6496, 18.9560).kind == SunKind::PolarDay);
  CHECK(ld::sun_times({2025, 12, 21}, 69.6496, 18.9560).kind == SunKind::PolarNight);
  const ld::SunTimes equinox = ld::sun_times({2026, 3, 21}, 69.6496, 18.9560);
  CHECK(equinox.kind == SunKind::Normal);
  CHECK(equinox.rise < equinox.set);
  // the pole itself: clamped to 89.9 degrees, no division by zero
  CHECK(ld::sun_times({2026, 6, 21}, 90.0, 0.0).kind == SunKind::PolarDay);
}

TEST_CASE("sun_view highlights the next event and switches to tomorrow after sunset") {
  const ld::LocalZone z = brussels_zone();
  const CivilDate day{2026, 10, 11};
  const ld::SunTimes s = ld::sun_times(day, kBxlLat, kBxlLon);
  const std::int64_t midnight = z.midnight({2026, 10, 12});
  CHECK(midnight == at({2026, 10, 11}, 22, 0));  // 00:00 CEST

  ld::SunView v = ld::sun_view(at(day, 4, 0), kBxlLat, kBxlLon, z, {});
  CHECK(v.next == ld::SunNext::Rise);
  CHECK(v.rise == s.rise);
  CHECK(v.change_at == s.rise);

  v = ld::sun_view(s.rise, kBxlLat, kBxlLon, z, {});  // the very second of sunrise: now the sunset
  CHECK(v.next == ld::SunNext::Set);
  CHECK(v.change_at == s.set);

  v = ld::sun_view(s.set, kBxlLat, kBxlLon, z, {});  // after sunset: tomorrow's times
  const ld::SunTimes tomorrow = ld::sun_times({2026, 10, 12}, kBxlLat, kBxlLon);
  CHECK(v.next == ld::SunNext::Rise);
  CHECK(v.rise == tomorrow.rise);
  CHECK(v.set == tomorrow.set);
  CHECK(v.change_at == midnight);  // refreshed at midnight even though nothing moves
}

TEST_CASE("sun_view applies the online correction") {
  const ld::LocalZone z = brussels_zone();
  const CivilDate day{2026, 10, 11};
  const ld::SunTimes s = ld::sun_times(day, kBxlLat, kBxlLon);
  const ld::SunView v = ld::sun_view(at(day, 4, 0), kBxlLat, kBxlLon, z, {300, -180});
  CHECK(v.rise == s.rise + 300);
  CHECK(v.set == s.set - 180);
  CHECK(v.change_at == s.rise + 300);
}

TEST_CASE("sun_view during a polar day changes only at midnight") {
  const ld::LocalZone z = brussels_zone();  // the zone does not matter for this check
  const ld::SunView v = ld::sun_view(at({2026, 6, 21}, 10, 0), 69.6496, 18.9560, z, {});
  CHECK(v.kind == SunKind::PolarDay);
  CHECK(v.change_at == z.midnight({2026, 6, 22}));
}

TEST_CASE("local times follow the DST change: sunrise jumps an hour on 29 March and back on 25 October") {
  const ld::LocalZone z = brussels_zone();
  auto rise_hour = [&](CivilDate d) {
    return z.to_local(ld::sun_view(at(d, 0, 0), kBxlLat, kBxlLon, z, {}).rise).tm_hour;
  };
  CHECK(rise_hour({2026, 3, 28}) == 6);  // 05:27 UTC, CET
  CHECK(rise_hour({2026, 3, 29}) == 7);  // 05:25 UTC, CEST
  CHECK(rise_hour({2026, 10, 24}) == 8); // 06:19 UTC, CEST
  CHECK(rise_hour({2026, 10, 25}) == 7); // 06:21 UTC, CET
  CHECK(z.midnight({2026, 3, 30}) - z.midnight({2026, 3, 29}) == 23 * 3600);
  CHECK(z.midnight({2026, 10, 26}) - z.midnight({2026, 10, 25}) == 25 * 3600);
}

TEST_CASE("format_hm rounds to the nearest minute in local time") {
  const ld::LocalZone z = brussels_zone();
  CHECK(ld::format_hm(at({2026, 10, 11}, 5, 52) + 29, z) == L"07:52");
  CHECK(ld::format_hm(at({2026, 10, 11}, 5, 52) + 30, z) == L"07:53");
  CHECK(ld::format_hm(at({2026, 12, 1}, 16, 41), z) == L"17:41");
  CHECK(ld::format_hm(at({2026, 10, 11}, 21, 59) + 45, z) == L"00:00");
}

TEST_CASE("parse_online_sun converts Open-Meteo's local times with utc_offset_seconds") {
  const char* body = R"({"latitude":50.854,"longitude":4.35,"utc_offset_seconds":7200,"timezone":"Europe/Brussels",
    "daily":{"time":["2026-10-11"],"sunrise":["2026-10-11T07:57"],"sunset":["2026-10-11T18:58"]}})";
  const auto o = ld::parse_online_sun(body);
  REQUIRE(o);
  CHECK(o->date == CivilDate{2026, 10, 11});
  CHECK(o->rise == at({2026, 10, 11}, 5, 57));
  CHECK(o->set == at({2026, 10, 11}, 16, 58));
  CHECK_FALSE(ld::parse_online_sun("not json"));
  CHECK_FALSE(ld::parse_online_sun(R"({"utc_offset_seconds":0,"daily":{"time":[],"sunrise":[],"sunset":[]}})"));
  CHECK_FALSE(ld::parse_online_sun(R"({"daily":{"time":["2026-10-11"],"sunrise":["x"],"sunset":["y"]}})"));
  CHECK_FALSE(ld::parse_online_sun(R"({"utc_offset_seconds":0,"daily":[]})"));
  CHECK_FALSE(ld::parse_online_sun(R"({"utc_offset_seconds":"x","daily":{"time":["2026-10-11"],"sunrise":["2026-10-11T07:57"],"sunset":["2026-10-11T18:58"]}})"));
  CHECK_FALSE(ld::parse_online_sun(R"({"utc_offset_seconds":0,"daily":{"time":["2026-10-11"],"sunrise":[null],"sunset":["2026-10-11T18:58"]}})"));
}

TEST_CASE("compare_sun: 2 minutes or less agree, more is a mismatch with the online minus local offset") {
  const ld::SunTimes local{SunKind::Normal, 1000, 50000};
  CHECK(ld::compare_sun(local, {{2026, 10, 11}, 1000 + 120, 50000 - 120}).agree);
  const ld::SunCheck off = ld::compare_sun(local, {{2026, 10, 11}, 1000 + 121, 50000});
  CHECK_FALSE(off.agree);
  CHECK(off.correction == ld::SunCorrection{121, 0});
  CHECK(ld::sun_check_url(50.8503, 4.3517) ==
        "https://api.open-meteo.com/v1/forecast?latitude=50.8503&longitude=4.3517"
        "&daily=sunrise,sunset&timezone=auto&forecast_days=1");
}

TEST_CASE("compare_sun: unusable answers apply no correction") {
  const ld::SunTimes local{SunKind::Normal, 100000, 150000};
  const CivilDate d{2026, 10, 25};
  const ld::SunCheck wrong_offset = ld::compare_sun(local, {d, 100000 + 3600, 150000});
  CHECK_FALSE(wrong_offset.usable);
  CHECK_FALSE(wrong_offset.agree);
  CHECK(wrong_offset.correction == ld::SunCorrection{0, 0});
  const ld::SunCheck close = ld::compare_sun(local, {d, 100000 + 119, 150000 - 119});
  CHECK(close.usable);
  CHECK(close.agree);
  const ld::SunCheck off = ld::compare_sun(local, {d, 100000 - 121, 150000 + 600});
  CHECK(off.usable);
  CHECK_FALSE(off.agree);
  CHECK(off.correction == ld::SunCorrection{-121, 600});
  for (SunKind k : {SunKind::PolarDay, SunKind::PolarNight}) {
    const ld::SunCheck polar = ld::compare_sun({k, 0, 0}, {d, 100000, 150000});
    CHECK_FALSE(polar.usable);
    CHECK_FALSE(polar.agree);
    CHECK(polar.correction == ld::SunCorrection{0, 0});
  }
}
