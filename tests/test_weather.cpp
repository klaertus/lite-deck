#include <doctest/doctest.h>
#include <format>
#include "core/http.h"
#include "modules/common/format.h"
#include "fake_context.h"
#include "module_fixtures.h"
#include "modules/common/wall_clock.h"
#include "modules/schema.h"
#include "modules/weather/normals.h"
#include "modules/weather/weather.h"

using namespace std::chrono_literals;

static const char* kGeo = R"({"results":[{"name":"Brussels","latitude":50.8503,"longitude":4.3517}]})";
static const char* kForecast = R"({"current":{"temperature_2m":12.6,"weather_code":3},
  "daily":{"temperature_2m_max":[15.2],"temperature_2m_min":[8.4]}})";

static std::unique_ptr<ld::Module> start(FakeContext& ctx, const char* city = "Brussels",
                                         const char* basis = "absolute") {
  auto info = ld::weather_module_info();
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json{{"city", city}, {"gradient_basis", basis}}, s));
  auto m = info.create();
  m->start(ctx, s);
  return m;
}

// An archive answer: every day of `first`..`last` at `celsius`.
static std::string archive(int first, int last, double celsius) {
  std::string time, mean;
  for (std::int64_t d = ld::days_from_civil({first, 1, 1}); d <= ld::days_from_civil({last, 12, 31}); ++d) {
    if (!time.empty()) {
      time += ',';
      mean += ',';
    }
    time += '"' + ld::format_iso_date(ld::civil_from_days(d)) + '"';
    mean += std::format("{:.1f}", celsius);
  }
  return R"({"daily":{"time":[)" + time + R"(],"temperature_2m_mean":[)" + mean + "]}}";
}

// The wall clock on a chosen local day, movable by whole days.
struct Today {
  std::chrono::system_clock::time_point t;
  Today() {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 12;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    t = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    ld::set_wall_clock([this] { return t; });
  }
  ~Today() { ld::set_wall_clock({}); }
  void next_day() { t += std::chrono::hours(24); }
};

TEST_CASE("weather parsers") {
  auto g = ld::parse_geocode(kGeo);
  REQUIRE(g);
  CHECK(g->lat == doctest::Approx(50.8503));
  CHECK_FALSE(ld::parse_geocode(R"({"generationtime_ms":0.1})"));
  CHECK_FALSE(ld::parse_geocode("not json"));
  auto w = ld::parse_forecast(kForecast);
  REQUIRE(w);
  CHECK(w->temp == doctest::Approx(12.6));
  CHECK(w->code == 3);
  CHECK(w->tmax == doctest::Approx(15.2));
  CHECK_FALSE(ld::parse_forecast(R"({"current":{}})"));
}

TEST_CASE("parse_geocode keeps the city's time zone when the geocoder gives one") {
  auto g = ld::parse_geocode(
      R"({"results":[{"name":"Brussels","latitude":50.8503,"longitude":4.3517,"timezone":"Europe/Brussels"}]})");
  REQUIRE(g);
  CHECK(g->timezone == "Europe/Brussels");
  CHECK(ld::parse_geocode(kGeo)->timezone.empty());
}

TEST_CASE("weather urls") {
  CHECK(ld::geocode_url("New York") ==
        "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=New%20York");
  CHECK(ld::forecast_url({50.8503, 4.3517}).starts_with(
        "https://api.open-meteo.com/v1/forecast?latitude=50.8503&longitude=4.3517&current=temperature_2m,weather_code"));
}

TEST_CASE("weather_icon maps WMO codes") {
  CHECK(ld::weather_icon(0) == L"☀️");
  CHECK(ld::weather_icon(3) == L"☁️");
  CHECK(ld::weather_icon(63) == L"🌧️");
  CHECK(ld::weather_icon(95) == L"⛈️");
}

TEST_CASE("weather geocodes once, caches the point and refreshes every 15 minutes") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  REQUIRE(ctx.requests.size() == 1);
  CHECK(ctx.requests[0].first.find("geocoding-api") != std::string::npos);
  ctx.respond(0, {200, kGeo, ""});
  REQUIRE(ctx.requests.size() == 2);
  CHECK(ctx.state["lat"] == doctest::Approx(50.8503));
  ctx.respond(1, {200, kForecast, ""});
  CHECK(ctx.invalidations >= 1);
  loop.advance(ld::kWeatherPeriod);
  REQUIRE(ctx.requests.size() == 3);
  CHECK(ctx.requests[2].first.find("api.open-meteo.com/v1/forecast") != std::string::npos);

  FakeContext ctx2(loop);  // a new instance with the cached point skips geocoding
  ctx2.state = ctx.state;
  auto m2 = start(ctx2);
  REQUIRE(ctx2.requests.size() == 1);
  CHECK(ctx2.requests[0].first.find("forecast") != std::string::npos);
}

TEST_CASE("weather keeps the last value greyed on error") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = start(ctx);
  ctx.respond(0, {200, kGeo, ""});
  ctx.respond(1, {200, kForecast, ""});
  int ok_pixels = render_lit_pixels(*m, r);
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(2, {429, "", ""});
  CHECK(render_lit_pixels(*m, r) > 0);
  CHECK(render_lit_pixels(*m, r) <= ok_pixels);  // greyed text is dimmer
  CHECK(ctx.requests.size() == 3);              // no fast retry
}

TEST_CASE("weather shows a hint when the city is unknown") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = start(ctx, "Zzyzxville");
  ctx.respond(0, {200, R"({"generationtime_ms":0.2})", ""});
  CHECK(ctx.requests.size() == 1);
  CHECK(render_lit_pixels(*m, r) > 5);
  loop.advance(ld::kWeatherPeriod);
  CHECK(ctx.requests.size() == 2);  // retried at the next period only
}

TEST_CASE("read_geocode tells a place, no match and an unreadable answer apart") {
  CHECK(ld::read_geocode(kGeo).status == ld::GeocodeStatus::Found);
  CHECK(ld::read_geocode(R"({"generationtime_ms":0.2})").status == ld::GeocodeStatus::NoMatch);
  CHECK(ld::read_geocode(R"({"results":[]})").status == ld::GeocodeStatus::NoMatch);
  for (const char* body : {"<html>login</html>", "", "[]", R"({"error":true,"reason":"x"})", R"({"results":"x"})",
                           R"({"results":[{"latitude":"x","longitude":4}]})"}) {
    INFO(body);
    CHECK(ld::read_geocode(body).status == ld::GeocodeStatus::Unreadable);
    CHECK_FALSE(ld::read_geocode(body).point);
  }
}

TEST_CASE("an unreadable geocoding answer is not an unknown city: no hint, retried at the next period") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  ctx.respond(0, {200, "<html>Hotel Wi-Fi login</html>", ""});
  CHECK_FALSE(fixtures::element_of(*m, "status"));
  CHECK(fixtures::element_of(*m, "temp")->text == ld::kDash);
  loop.advance(ld::kWeatherPeriod);
  REQUIRE(ctx.requests.size() == 2);
  CHECK(ctx.requests[1].first.find("geocoding-api") != std::string::npos);
  ctx.respond(1, {200, kGeo, ""});
  ctx.respond(2, {200, kForecast, ""});
  CHECK(fixtures::element_of(*m, "temp")->text == L"13°");
}

TEST_CASE("a cached city of the wrong type is ignored, not thrown on") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ctx.state = nlohmann::json{{"city", 123}, {"lat", 50.8503}, {"lon", 4.3517}};
  std::unique_ptr<ld::Module> m;
  CHECK_NOTHROW(m = start(ctx));
  REQUIRE(ctx.requests.size() == 1);
  CHECK(ctx.requests[0].first.find("geocoding-api") != std::string::npos);
}

TEST_CASE("weather's default city is empty: a hint until one is set") {
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(ld::weather_module_info().settings_schema, nlohmann::json::object(), s));
  CHECK(s["city"] == "");
}

TEST_CASE("weather with an empty city shows a hint and never requests") {
  for (const char* city : {"", "   "}) {
    FakeLoop loop;
    FakeContext ctx(loop);
    ld::Renderer r;
    auto info = ld::weather_module_info();
    auto m = info.create();
    m->start(ctx, nlohmann::json{{"city", city}, {"color", "#FFFFFF"}});
    CHECK(ctx.requests.empty());
    loop.advance(ld::kWeatherPeriod);
    CHECK(ctx.requests.empty());
    CHECK(render_lit_pixels(*m, r) > 5);
  }
}

TEST_CASE("weather ignores a corrupt cached point") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ctx.state = nlohmann::json{{"city", "Brussels"}, {"lat", "x"}, {"lon", nullptr}};
  std::unique_ptr<ld::Module> m;
  CHECK_NOTHROW(m = start(ctx));
  REQUIRE(ctx.requests.size() == 1);
  CHECK(ctx.requests[0].first.find("geocoding-api") != std::string::npos);
}

TEST_CASE("weather does not invalidate when the forecast is unchanged") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  ctx.respond(0, {200, kGeo, ""});
  ctx.respond(1, {200, kForecast, ""});
  int after_first = ctx.invalidations;
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(2, {200, kForecast, ""});  // same values
  CHECK(ctx.invalidations == after_first);
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(3, {429, "", ""});          // goes stale (grey)
  CHECK(ctx.invalidations == after_first + 1);
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(4, {200, kForecast, ""});  // same values, but no longer stale
  CHECK(ctx.invalidations == after_first + 2);
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(5, {200, R"({"current":{"temperature_2m":14.2,"weather_code":3},
    "daily":{"temperature_2m_max":[15.2],"temperature_2m_min":[8.4]}})", ""});
  CHECK(ctx.invalidations == after_first + 3);
}

TEST_CASE("weather: three elements, a gradient metric, stale when the forecast fails; no colour setting") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  ctx.respond(0, {200, kGeo, ""});
  ctx.respond(1, {200, kForecast, ""});
  const auto temp = fixtures::element_of(*m, "temp");
  REQUIRE(temp);
  CHECK(temp->text == L"13°");
  REQUIRE(temp->metric);
  CHECK(*temp->metric == doctest::Approx(12.6));      // no normals yet: the absolute temperature
  CHECK(temp->gradient == ld::kWeatherAbsoluteGradient);
  CHECK(fixtures::element_of(*m, "icon")->text == ld::weather_icon(3));
  CHECK(fixtures::element_of(*m, "range")->text == L"8° / 15°");
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(2, {429, "", ""});
  CHECK(fixtures::element_of(*m, "temp")->state == ld::ElementState::Stale);
  CHECK(fixtures::element_of(*m, "range")->state == ld::ElementState::Stale);
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(ld::weather_module_info().settings_schema, nlohmann::json::object(), s));
  CHECK_FALSE(s.contains("color"));
  CHECK(s["gradient_basis"] == "anomaly");
  CHECK(s["cold_at"] == -8.0);
  CHECK(s["hot_color"] == "#ff5a3c");
}

TEST_CASE("with normals the temperature colour follows its gap to the seasonal normal, cached in the state") {
  Today today;
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, "Brussels", "anomaly");
  ctx.respond(0, {200, kGeo, ""});
  ctx.respond(1, {200, kForecast, ""});
  REQUIRE(ctx.requests.size() == 3);
  CHECK(ctx.requests[2].first == ld::normals_url(50.8503, 4.3517, 1996, 2025));
  const int before = ctx.invalidations;
  ctx.respond(2, {200, archive(2024, 2025, 10.0), ""});
  CHECK(ctx.invalidations == before + 1);  // the colour changed
  const auto temp = fixtures::element_of(*m, "temp");
  REQUIRE(temp->metric);
  CHECK(*temp->metric == doctest::Approx(1.8));  // a daily mean of 11.8 °C (8.4 / 15.2) against a normal of 10 °C
  CHECK(temp->gradient == ld::kWeatherAnomalyGradient);
  REQUIRE(ctx.state.contains("normals"));
  CHECK(ctx.state["normals"]["fetched"] == "2026-10-12");
  CHECK(ctx.state["city"] == "Brussels");  // the point is kept with the normals

  FakeContext ctx2(loop);  // a restart within the month: no new archive request
  ctx2.state = ctx.state;
  auto m2 = start(ctx2, "Brussels", "anomaly");
  REQUIRE(ctx2.requests.size() == 1);
  ctx2.respond(0, {200, kForecast, ""});
  CHECK(ctx2.requests.size() == 1);
  CHECK(*fixtures::element_of(*m2, "temp")->metric == doctest::Approx(1.8));
}

TEST_CASE("normals older than a month are asked for again") {
  Today today;
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::CachedNormals old;
  old.values.fill(10.0);
  old.fetched = ld::CivilDate{2026, 9, 1};
  ctx.state = {{"city", "Brussels"}, {"lat", 50.8503}, {"lon", 4.3517}, {"normals", ld::normals_to_json(old)}};
  auto m = start(ctx, "Brussels", "anomaly");
  ctx.respond(0, {200, kForecast, ""});
  REQUIRE(ctx.requests.size() == 2);
  CHECK(ctx.requests[1].first.find("archive-api") != std::string::npos);
  CHECK(*fixtures::element_of(*m, "temp")->metric == doctest::Approx(1.8));  // the old ones still serve
}

TEST_CASE("with normals the colour follows the daily mean, not the hour: stable from dawn to afternoon") {
  Today today;
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::CachedNormals fresh;
  fresh.values.fill(10.0);
  fresh.fetched = ld::CivilDate{2026, 10, 12};
  ctx.state = {{"city", "Brussels"}, {"lat", 50.8503}, {"lon", 4.3517}, {"normals", ld::normals_to_json(fresh)}};
  auto m = start(ctx, "Brussels", "anomaly");
  ctx.respond(0, {200, R"({"current":{"temperature_2m":6.1,"weather_code":0},
    "daily":{"temperature_2m_max":[16.0],"temperature_2m_min":[6.0]}})", ""});  // dawn
  const auto dawn = fixtures::element_of(*m, "temp");
  CHECK(dawn->text == L"6°");
  CHECK(*dawn->metric == doctest::Approx(1.0));  // (6 + 16) / 2 = 11 against 10
  CHECK(dawn->gradient == ld::kWeatherAnomalyGradient);
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(1, {200, R"({"current":{"temperature_2m":15.8,"weather_code":0},
    "daily":{"temperature_2m_max":[16.0],"temperature_2m_min":[6.0]}})", ""});  // afternoon
  const auto afternoon = fixtures::element_of(*m, "temp");
  CHECK(afternoon->text == L"16°");
  CHECK(*afternoon->metric == doctest::Approx(1.0));  // same colour
}

TEST_CASE("normals fetched in the future (clock change, hand edit) count as stale") {
  Today today;
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::CachedNormals odd;
  odd.values.fill(10.0);
  odd.fetched = ld::CivilDate{2027, 3, 1};
  ctx.state = {{"city", "Brussels"}, {"lat", 50.8503}, {"lon", 4.3517}, {"normals", ld::normals_to_json(odd)}};
  auto m = start(ctx, "Brussels", "anomaly");
  ctx.respond(0, {200, kForecast, ""});
  REQUIRE(ctx.requests.size() == 2);
  CHECK(ctx.requests[1].first.find("archive-api") != std::string::npos);
}

TEST_CASE("without normals the weather falls back to the absolute gradient and retries the next day") {
  Today today;
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, "Brussels", "anomaly");
  ctx.respond(0, {200, kGeo, ""});
  ctx.respond(1, {200, kForecast, ""});
  ctx.respond(2, {0, "", "timeout"});
  const auto temp = fixtures::element_of(*m, "temp");
  CHECK(*temp->metric == doctest::Approx(12.6));
  CHECK(temp->gradient == ld::kWeatherAbsoluteGradient);
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(3, {200, kForecast, ""});
  CHECK(ctx.requests.size() == 4);  // not again today
  today.next_day();
  loop.advance(ld::kWeatherPeriod);
  ctx.respond(4, {200, kForecast, ""});
  REQUIRE(ctx.requests.size() == 6);
  CHECK(ctx.requests[5].first.find("archive-api") != std::string::npos);
}

TEST_CASE("a response too large asks for 20 then 10 years, then waits for tomorrow") {
  Today today;
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, "Brussels", "anomaly");
  ctx.respond(0, {200, kGeo, ""});
  ctx.respond(1, {200, kForecast, ""});
  ctx.respond(2, {200, "", ld::kResponseTooLarge});
  REQUIRE(ctx.requests.size() == 4);
  CHECK(ctx.requests[3].first == ld::normals_url(50.8503, 4.3517, 2006, 2025));
  ctx.respond(3, {200, "", ld::kResponseTooLarge});
  REQUIRE(ctx.requests.size() == 5);
  CHECK(ctx.requests[4].first == ld::normals_url(50.8503, 4.3517, 2016, 2025));
  ctx.respond(4, {200, "", ld::kResponseTooLarge});
  CHECK(ctx.requests.size() == 5);  // 10 years is the floor: failed for today
  CHECK(fixtures::element_of(*m, "temp")->gradient == ld::kWeatherAbsoluteGradient);
}
