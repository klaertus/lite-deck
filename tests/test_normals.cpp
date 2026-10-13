// tests/test_normals.cpp: seasonal normals from daily means, pure.
#include <doctest/doctest.h>
#include <string>
#include "modules/weather/normals.h"

using namespace ld;

namespace {
// `years` complete years of daily means, value(slot) for each day.
template <class F>
std::vector<std::pair<CivilDate, double>> series(int first, int last, F value) {
  std::vector<std::pair<CivilDate, double>> out;
  for (std::int64_t d = days_from_civil({first, 1, 1}); d <= days_from_civil({last, 12, 31}); ++d) {
    const CivilDate c = civil_from_days(d);
    out.emplace_back(c, value(c));
  }
  return out;
}
}  // namespace

TEST_CASE("day slots follow a leap year: 29 February has its own") {
  CHECK(day_slot(1, 1) == 0);
  CHECK(day_slot(2, 28) == 58);
  CHECK(day_slot(2, 29) == 59);
  CHECK(day_slot(3, 1) == 60);
  CHECK(day_slot(12, 31) == 365);
}

TEST_CASE("the archive URL asks for daily means over whole years") {
  const std::string url = normals_url(50.8503, 4.3517, 1996, 2025);
  CHECK(url ==
        "https://archive-api.open-meteo.com/v1/archive?latitude=50.8503&longitude=4.3517"
        "&start_date=1996-01-01&end_date=2025-12-31&daily=temperature_2m_mean&timezone=auto");
}

TEST_CASE("daily means are read from a real answer; nulls are skipped; a broken answer is refused") {
  const char* real =
      R"({"latitude":50.65026,"longitude":4.6276593,"generationtime_ms":0.08392333984375,"utc_offset_seconds":7200,)"
      R"("timezone":"Europe/Brussels","timezone_abbreviation":"GMT+2","elevation":59.0,)"
      R"("daily_units":{"time":"iso8601","temperature_2m_mean":"°C"},"daily":{"time":["2024-02-27","2024-02-28",)"
      R"("2024-02-29","2024-03-01","2024-03-02"],"temperature_2m_mean":[4.5,4.2,7.0,7.4,7.9]}})";
  const auto days = parse_daily_means(real);
  REQUIRE(days);
  REQUIRE(days->size() == 5);
  CHECK((*days)[2].first == CivilDate{2024, 2, 29});
  CHECK((*days)[2].second == 7.0);
  const auto holes = parse_daily_means(R"({"daily":{"time":["2024-01-01","2024-01-02"],"temperature_2m_mean":[null,3.5]}})");
  REQUIRE(holes);
  CHECK(holes->size() == 1);
  CHECK_FALSE(parse_daily_means(R"({"daily":{"time":["2024-01-01"],"temperature_2m_mean":[]}})"));
  CHECK_FALSE(parse_daily_means(R"({"error":true,"reason":"bad"})"));
  CHECK_FALSE(parse_daily_means("not json"));
}

TEST_CASE("a normal is the mean of the days within seven days, across the years, wrapping at the year's end") {
  const auto days = series(2023, 2024, [](CivilDate c) { return c.month == 1 && c.day == 1 ? 15.0 : 0.0; });
  const auto n = compute_normals(days);
  REQUIRE(n);
  CHECK((*n)[0] == doctest::Approx(1.0));    // 30 / 30 values
  CHECK((*n)[7] == doctest::Approx(1.0));    // 1 January is 7 days back
  CHECK((*n)[8] == doctest::Approx(0.0));    // 8 days: outside
  CHECK((*n)[359] == doctest::Approx(1.0));  // 24 December: 1 January is 7 days ahead
  CHECK((*n)[358] == doctest::Approx(0.0));
  const auto flat = compute_normals(series(2020, 2021, [](CivilDate) { return 10.0; }));
  REQUIRE(flat);
  for (double v : *flat) CHECK(v == doctest::Approx(10.0));
}

TEST_CASE("leap years: 29 February counts once per leap year; normal_for maps dates to slots") {
  const auto days = series(2023, 2024, [](CivilDate c) { return c.month == 2 && c.day == 29 ? 30.0 : 0.0; });
  const auto n = compute_normals(days);
  REQUIRE(n);
  CHECK((*n)[59] == doctest::Approx(30.0 / 29.0));  // 14 slots x 2 years + the one 29 February
  CHECK(normal_for(*n, CivilDate{2024, 2, 29}) == (*n)[59]);
  CHECK(normal_for(*n, CivilDate{2026, 3, 1}) == (*n)[60]);
}

TEST_CASE("too little data gives no normals") {
  CHECK_FALSE(compute_normals({}));
  auto january = series(2024, 2024, [](CivilDate) { return 1.0; });
  january.resize(31);  // 1 to 31 January only: most slots have nothing within seven days
  CHECK_FALSE(compute_normals(january));
}

TEST_CASE("the cache round-trips at 0.1 °C and refuses a damaged array") {
  CachedNormals c;
  c.values.fill(10.04);
  c.values[59] = -3.26;
  c.fetched = CivilDate{2026, 10, 12};
  c.years = 20;
  const nlohmann::json j = normals_to_json(c);
  CHECK(j["fetched"] == "2026-10-12");
  CHECK(j["years"] == 20);
  REQUIRE(j["values"].size() == 366);
  const auto back = normals_from_json(j);
  REQUIRE(back);
  CHECK(back->values[0] == doctest::Approx(10.0));
  CHECK(back->values[59] == doctest::Approx(-3.3));
  CHECK(back->fetched == CivilDate{2026, 10, 12});
  CHECK(back->years == 20);
  nlohmann::json bad = j;
  bad["values"].erase(0);
  CHECK_FALSE(normals_from_json(bad));
  bad = j;
  bad["values"][3] = "x";
  CHECK_FALSE(normals_from_json(bad));
  CHECK_FALSE(normals_from_json(nlohmann::json()));
}
