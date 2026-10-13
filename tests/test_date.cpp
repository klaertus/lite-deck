#include <doctest/doctest.h>
#include "fake_context.h"
#include "module_fixtures.h"
#include "modules/date/date.h"
#include "modules/schema.h"

using namespace std::chrono_literals;

TEST_CASE("date_text uses English names") {
  std::tm t{};
  t.tm_wday = 6;   // Saturday
  t.tm_mday = 11;
  t.tm_mon = 9;    // October
  auto d = ld::date_text(t);
  CHECK(d.weekday == L"Saturday");
  CHECK(d.day == L"11");
  CHECK(d.month == L"Oct");
  t.tm_wday = 0;
  t.tm_mon = 7;
  CHECK(ld::date_text(t).weekday == L"Sunday");
  CHECK(ld::date_text(t).month == L"Aug");
}

TEST_CASE("until_next_midnight counts the rest of the day") {
  std::tm t{};
  t.tm_year = 126;  // 2026
  t.tm_mon = 9;     // October
  t.tm_mday = 14;   // no DST change in Europe or the US that night
  t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 30;
  CHECK(ld::until_next_midnight(t) == 30000ms);
  t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
  CHECK(ld::until_next_midnight(t) == 86400000ms);
}

// A fake local clock where the 29th springs forward at 02:00 (a 23-hour day) and the 30th falls back (25 hours).
static std::time_t fake_mktime(std::tm* t) {
  long long s = (t->tm_mday * 24LL + t->tm_hour) * 3600 + t->tm_min * 60LL + t->tm_sec;
  if (t->tm_mday > 29 || (t->tm_mday == 29 && t->tm_hour >= 3)) s -= 3600;  // after spring forward
  if (t->tm_mday > 30 || (t->tm_mday == 30 && t->tm_hour >= 3)) s += 3600;  // after fall back
  return static_cast<std::time_t>(s);
}

TEST_CASE("until_next_midnight follows DST changes through mktime") {
  std::tm t{};
  t.tm_mday = 29;  // 00:00 on a 23-hour day
  CHECK(ld::until_next_midnight(t, &fake_mktime) == 23h);
  t.tm_mday = 30;  // 00:00 on a 25-hour day
  CHECK(ld::until_next_midnight(t, &fake_mktime) == 25h);
  t.tm_mday = 28;
  t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 30;
  CHECK(ld::until_next_midnight(t, &fake_mktime) == 30s);
}

TEST_CASE("date module renders and schedules a single midnight timer") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto info = ld::date_module_info();
  auto m = info.create();
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  m->start(ctx, s);
  CHECK(render_lit_pixels(*m, r) > 20);
  CHECK(loop.sched.size() == 1);
}

TEST_CASE("date catches a wall-clock jump within 10 minutes and redraws only when the day changed") {
  FakeLoop loop;
  FakeContext ctx(loop);
  std::tm tm{};
  tm.tm_year = 2026 - 1900;
  tm.tm_mon = 9;
  tm.tm_mday = 14;
  tm.tm_hour = 13;
  tm.tm_isdst = -1;
  const auto base = std::chrono::system_clock::from_time_t(std::mktime(&tm));
  std::chrono::hours jump{0};
  ld::set_wall_clock([&] {
    return base + jump + std::chrono::duration_cast<std::chrono::system_clock::duration>(loop.now().time_since_epoch());
  });
  struct WallClockBack {
    ~WallClockBack() { ld::set_wall_clock({}); }
  } back;
  auto info = ld::date_module_info();
  auto m = info.create();
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  m->start(ctx, s);
  const int first = ctx.invalidations;
  loop.advance(ld::kDateMaxSleep * 3);  // same day: the wake-ups redraw nothing
  CHECK(ctx.invalidations == first);
  jump = std::chrono::hours(24);  // NTP fixes a clock that booted a day behind
  loop.advance(ld::kDateMaxSleep + 1s);
  CHECK(ctx.invalidations == first + 1);
  CHECK(fixtures::element_of(*m, "day")->text == L"15");
  CHECK(loop.sched.size() == 1);
}

TEST_CASE("date shows the weekday, the day and the month as three elements") {
  fixtures::FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  auto rig = fixtures::date_rig();
  CHECK(fixtures::element_of(*rig->m, "weekday")->text == L"Wednesday");
  CHECK(fixtures::element_of(*rig->m, "day")->text == L"14");
  CHECK(fixtures::element_of(*rig->m, "month")->text == L"Oct");
}
