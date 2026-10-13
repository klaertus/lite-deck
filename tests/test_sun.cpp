// tests/test_sun.cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>
#include <format>
#include "fake_context.h"
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/wall_clock.h"
#include "modules/schema.h"
#include "modules/sun/sun.h"
#include "modules/weather/weather.h"
#include "sun_zone.h"

using namespace std::chrono_literals;

namespace {

constexpr double kLat = 50.8503, kLon = 4.3517;  // Brussels
const char* kGeo =
    R"({"results":[{"name":"Brussels","latitude":50.8503,"longitude":4.3517,"timezone":"Europe/Brussels"}]})";
// CEST. Sunrise about 05:58 UTC (07:58 local), sunset about 16:58 UTC (18:58 local).
const ld::CivilDate kDay{2026, 10, 11};

std::int64_t at_utc(ld::CivilDate d, int h, int m) { return ld::days_from_civil(d) * 86400 + h * 3600LL + m * 60LL; }

// Open-Meteo's answer for kDay (local times, CEST). The comparison uses the date of the answer.
std::string online(const char* rise, const char* set) {
  return std::format(R"({{"utc_offset_seconds":7200,"timezone":"Europe/Brussels",
    "daily":{{"time":["2026-10-11"],"sunrise":["2026-10-11T{}"],"sunset":["2026-10-11T{}"]}}}})",
                     rise, set);
}

// The state a previous run left for Brussels.
nlohmann::json cached(const char* verified_on = nullptr) {
  nlohmann::json st{{"city", "Brussels"}, {"lat", kLat}, {"lon", kLon}, {"timezone", "Europe/Brussels"}};
  if (verified_on) st["verified_on"] = verified_on;
  return st;
}

// A sun key on a fake loop. Its wall clock starts at `start_unix` and moves with the loop; `jump_s`
// simulates a manual clock change. The time zone is Brussels, whatever the PC running the tests.
struct SunRig {
  FakeLoop loop;
  FakeContext ctx{loop};
  std::int64_t start_unix;
  std::int64_t jump_s = 0;
  std::unique_ptr<ld::Module> m;
  explicit SunRig(std::int64_t start) : start_unix(start) {
    ld::set_wall_clock([this] { return std::chrono::system_clock::time_point(std::chrono::seconds(now())); });
  }
  ~SunRig() { ld::set_wall_clock({}); }
  SunRig(const SunRig&) = delete;
  SunRig& operator=(const SunRig&) = delete;
  std::int64_t now() const {
    return start_unix + jump_s +
           std::chrono::duration_cast<std::chrono::seconds>(loop.now().time_since_epoch()).count();
  }
  void start(nlohmann::json over = nlohmann::json::object()) {
    if (!over.contains("city")) over["city"] = "Brussels";  // the default city is empty
    nlohmann::json s;
    REQUIRE_FALSE(ld::apply_schema(ld::sun_module_info().settings_schema, over, s));
    m = ld::make_sun_module(brussels_zone());
    m->start(ctx, s);
  }
};

// The highlight colour (#ffb74d) on the digits of the top line (sunrise) or the bottom line (sunset).
// The digits only, right of the arrow.
int amber_top(ld::Surface& s) { return saturated(s, 60, 14, 106, 54); }
int amber_bottom(ld::Surface& s) { return saturated(s, 60, 62, 106, 102); }

// Coloured pixels (channels spread by more than 80) in [x0, x1) x [y0, y1), split by hue: blue
// dominant, or warm (red and green above blue: the yellow of a colour emoji sun).
struct Hues {
  int blue = 0, warm = 0;
};
Hues hues(ld::Surface& s, int x0, int y0, int x1, int y1) {
  const std::vector<std::uint8_t> px = s.pixels();  // BGRA
  Hues h;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * s.width() + x) * 4;
      const int b = p[0], g = p[1], r = p[2];
      if (std::max({r, g, b}) - std::min({r, g, b}) <= 80) continue;
      if (b > r && b > g) ++h.blue;
      else if (r > b && g > b) ++h.warm;
    }
  return h;
}

// The blue dot of the online times: blue pixels at the top right, above the first line.
int dot(ld::Surface& s) { return hues(s, 88, 4, 112, 22).blue; }

}  // namespace

TEST_CASE("sun geocodes the city once and caches its coordinates and time zone") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));  // 04:00 local
  rig.start();
  REQUIRE(rig.ctx.requests.size() == 1);
  CHECK(rig.ctx.requests[0].first == ld::geocode_url("Brussels"));
  CHECK(ink(*draw(*rig.m, r)).count > 0);  // a dash while waiting
  rig.ctx.respond(0, {200, kGeo, ""});
  CHECK(rig.ctx.state["city"] == "Brussels");
  CHECK(rig.ctx.state["lat"].get<double>() == doctest::Approx(kLat));
  CHECK(rig.ctx.state["timezone"] == "Europe/Brussels");
  REQUIRE(rig.ctx.requests.size() == 2);  // then the first online check
  CHECK(rig.ctx.requests[1].first == ld::sun_check_url(kLat, kLon));
  CHECK(amber_top(*draw(*rig.m, r)) > 0);  // the times are shown without waiting for the check

  SunRig again(at_utc(kDay, 2, 0));  // the daemon restarts
  again.ctx.state = rig.ctx.state;
  again.start();
  REQUIRE(again.ctx.requests.size() == 1);
  CHECK(again.ctx.requests[0].first == ld::sun_check_url(kLat, kLon));  // no second geocoding
}

TEST_CASE("a new city is geocoded again and its old check is forgotten") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached("2026-10-10");
  rig.ctx.state["city"] = "Namur";
  rig.ctx.state["correction"] = {{"rise_s", 300}, {"set_s", 0}};
  rig.start();  // the setting says Brussels
  REQUIRE(rig.ctx.requests.size() == 1);
  CHECK(rig.ctx.requests[0].first == ld::geocode_url("Brussels"));
  rig.ctx.respond(0, {200, kGeo, ""});
  CHECK(rig.ctx.state["city"] == "Brussels");
  CHECK_FALSE(rig.ctx.state.contains("verified_on"));
  CHECK_FALSE(rig.ctx.state.contains("correction"));
}

TEST_CASE("an unknown city shows City? and never asks again") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));
  rig.start({{"city", "Nowhereville"}});
  rig.ctx.respond(0, {200, R"({"results":[]})", ""});
  const int after = rig.ctx.invalidations;
  CHECK(ink(*draw(*rig.m, r)).count > 0);  // "City?"
  rig.loop.advance(48h);
  CHECK(rig.ctx.requests.size() == 1);
  CHECK(rig.ctx.invalidations == after);

  SunRig blank(at_utc(kDay, 2, 0));
  blank.start({{"city", "  "}});
  CHECK(blank.ctx.requests.empty());
  CHECK(ink(*draw(*blank.m, r)).count > 0);
}

TEST_CASE("the key highlights sunrise, then sunset right after sunrise, then tomorrow after sunset") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached("2026-10-11");  // checked today: no request
  rig.start();
  CHECK(rig.ctx.requests.empty());
  auto before = draw(*rig.m, r);
  CHECK(amber_top(*before) > 0);
  CHECK(amber_bottom(*before) == 0);
  const int base = rig.ctx.invalidations;
  const ld::SunTimes today = ld::sun_times(kDay, kLat, kLon);
  rig.loop.advance(std::chrono::seconds(today.rise - rig.now() - 2));
  CHECK(rig.ctx.invalidations == base);  // the 10-minute wake-ups redraw nothing
  rig.loop.advance(3s);
  CHECK(rig.ctx.invalidations == base + 1);
  auto after = draw(*rig.m, r);
  CHECK(amber_top(*after) == 0);
  CHECK(amber_bottom(*after) > 0);
  rig.loop.advance(std::chrono::seconds(today.set - rig.now() + 2));
  CHECK(rig.ctx.invalidations == base + 2);  // tomorrow's times, sunrise highlighted
  CHECK(amber_top(*draw(*rig.m, r)) > 0);
  rig.loop.advance(std::chrono::seconds(at_utc(kDay, 22, 0) - rig.now() + 5));  // past local midnight
  CHECK(rig.ctx.invalidations == base + 2);  // same times: nothing to redraw
}

TEST_CASE("sun shows Polar day above the polar circle and skips the online comparison") {
  ld::Renderer r;
  SunRig rig(at_utc({2026, 6, 21}, 10, 0));
  rig.ctx.state = {{"city", "Tromso"}, {"lat", 69.6496}, {"lon", 18.9560}, {"timezone", "Europe/Oslo"}};
  rig.start({{"city", "Tromso"}});
  CHECK(ink(*draw(*rig.m, r)).count > 0);  // "Polar day"
  REQUIRE(rig.ctx.requests.size() == 1);
  rig.ctx.respond(0, {200, R"({"utc_offset_seconds":7200,"daily":{"time":["2026-06-21"],
    "sunrise":["2026-06-21T00:00"],"sunset":["2026-06-22T00:00"]}})", ""});
  CHECK(rig.ctx.state["verified_on"] == "2026-06-21");
  CHECK_FALSE(rig.ctx.state.contains("correction"));
}

TEST_CASE("an online check that agrees is dated in the state and repeated after verify_days") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached();
  rig.start();
  REQUIRE(rig.ctx.requests.size() == 1);
  rig.ctx.respond(0, {200, online("07:57", "18:58"), ""});  // within 2 min of the computed times
  CHECK(rig.ctx.state["verified_on"] == "2026-10-11");
  CHECK_FALSE(rig.ctx.state.contains("correction"));
  rig.loop.advance(24h * 6);
  CHECK(rig.ctx.requests.size() == 1);
  rig.loop.advance(24h);
  CHECK(rig.ctx.requests.size() == 2);  // 2026-10-18, seven days later
}

TEST_CASE("a mismatch over 2 min shows the online times with a dot until the next check") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached();
  rig.start();
  auto local = draw(*rig.m, r);
  CHECK(dot(*local) == 0);
  rig.ctx.respond(0, {200, online("08:03", "18:58"), ""});  // sunrise 5 min later than computed
  const ld::SunTimes local_times = ld::sun_times(kDay, kLat, kLon);
  REQUIRE(rig.ctx.state.contains("correction"));
  CHECK(rig.ctx.state["correction"]["rise_s"] == at_utc(kDay, 6, 3) - local_times.rise);  // about +5 min
  CHECK(rig.ctx.state["correction"]["set_s"] == at_utc(kDay, 16, 58) - local_times.set);  // under a minute
  CHECK(rig.ctx.state["verified_on"] == "2026-10-11");
  auto corrected = draw(*rig.m, r);
  CHECK(dot(*corrected) > 0);
  CHECK_FALSE(same_rows(*local, *corrected, 14, 54));  // 08:03 instead of 07:58
  const nlohmann::json saved = rig.ctx.state;

  rig.loop.advance(24h * 7);  // the next check agrees: the dot goes
  REQUIRE(rig.ctx.requests.size() == 2);
  rig.ctx.respond(1, {200, online("07:57", "18:58"), ""});
  CHECK_FALSE(rig.ctx.state.contains("correction"));
  CHECK(dot(*draw(*rig.m, r)) == 0);

  SunRig restarted(at_utc(kDay, 3, 0));  // a restart the day of the mismatch keeps the online times
  restarted.ctx.state = saved;
  restarted.start();
  CHECK(restarted.ctx.requests.empty());
  CHECK(dot(*draw(*restarted.m, r)) > 0);
}

TEST_CASE("a failed check keeps the local times and is retried the next day") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached();
  rig.start();
  auto shown = draw(*rig.m, r);
  const int inv = rig.ctx.invalidations;
  rig.ctx.respond(0, {0, "", "timeout"});
  CHECK_FALSE(rig.ctx.state.contains("verified_on"));
  CHECK(same_rows(*shown, *draw(*rig.m, r), 0, 112));  // the display did not move
  CHECK(rig.ctx.invalidations == inv);
  rig.loop.advance(19h);  // 23:00 local
  CHECK(rig.ctx.requests.size() == 1);
  rig.loop.advance(90min);  // 00:30 the next day
  CHECK(rig.ctx.requests.size() == 2);
}

TEST_CASE("verify_days 0 never checks online") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached();
  rig.start({{"verify_days", 0}});
  rig.loop.advance(24h * 31);
  CHECK(rig.ctx.requests.empty());
}

TEST_CASE("a verification date in the future does not block the check") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached("2027-01-01");  // written while the clock was wrong
  rig.start();
  REQUIRE(rig.ctx.requests.size() == 1);
  CHECK(rig.ctx.requests[0].first == ld::sun_check_url(kLat, kLon));
}

TEST_CASE("a clock change is caught within 10 minutes") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached("2026-10-11");
  rig.start();
  const int base = rig.ctx.invalidations;
  rig.jump_s = 10 * 3600;  // the clock is set to 14:00: sunrise has passed
  rig.loop.advance(601s);
  CHECK(rig.ctx.invalidations == base + 1);  // sunset highlighted now
}

TEST_CASE("sun is a key-only module with its settings and seven elements") {
  const auto info = ld::sun_module_info();
  CHECK(info.id == "sun");
  CHECK(info.allows(ld::SlotKind::Key));
  CHECK_FALSE(info.allows(ld::SlotKind::StripZone));
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  CHECK(s["city"] == "");  // no default town
  CHECK(s["verify_days"] == 7);
  CHECK_FALSE(s.contains("color"));
  nlohmann::json bad;
  CHECK(ld::apply_schema(info.settings_schema, nlohmann::json{{"verify_days", 31}}, bad));  // 0 to 30
  REQUIRE(info.elements.size() == 7);
  CHECK(std::string(info.elements[0].key) == "status");
  CHECK(std::string(info.elements[3].key) == "line1");
  CHECK(std::string(info.elements[5].key) == "line2");
  CHECK(std::string(info.elements[6].key) == "online");
}

TEST_CASE("the arrows take the highlight colour or grey, never a colour emoji") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached("2026-10-11");
  rig.start();
  ld::SlotLook blue;  // what the colour setting #2060ff did in a v1 config
  for (const char* key : {"symbol1", "line1", "symbol2", "line2"})
    blue.elements[key].color = ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{0x20, 0x60, 0xff}};
  auto s = draw(*rig.m, r, blue);
  const Ink top = ink(*s, 0, 10, -1, 54);
  REQUIRE(top.count > 0);
  // The arrow, left of the digits, is drawn in the highlight colour like the digits.
  CHECK(hues(*s, top.left, 10, top.left + 8, 54).blue > 0);
  CHECK(hues(*s, 0, 0, 112, 112).warm == 0);
  // The sunset line is grey from end to end, arrow included.
  const Ink bottom = ink(*s, 0, 58, -1, 102);
  REQUIRE(bottom.count > 0);
  CHECK(saturated(*s, 0, 58, 112, 102) == 0);
}

TEST_CASE("a failed geocoding is retried every 10 minutes until it succeeds") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.start();
  REQUIRE(rig.ctx.requests.size() == 1);
  rig.ctx.respond(0, {0, "", "no network"});
  CHECK(rig.ctx.state.is_null());
  rig.loop.advance(599s);
  CHECK(rig.ctx.requests.size() == 1);
  rig.loop.advance(2s);
  REQUIRE(rig.ctx.requests.size() == 2);
  CHECK(rig.ctx.requests[1].first == ld::geocode_url("Brussels"));
  rig.ctx.respond(1, {503, "", ""});
  rig.loop.advance(600s);
  REQUIRE(rig.ctx.requests.size() == 3);
  rig.ctx.respond(2, {200, kGeo, ""});
  CHECK(rig.ctx.state["city"] == "Brussels");
  REQUIRE(rig.ctx.requests.size() == 4);
  CHECK(rig.ctx.requests[3].first == ld::sun_check_url(kLat, kLon));
}

TEST_CASE("an unusable online answer applies no correction and is retried the next day") {
  ld::Renderer r;
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached();
  rig.start();
  auto shown = draw(*rig.m, r);
  const int inv = rig.ctx.invalidations;
  REQUIRE(rig.ctx.requests.size() == 1);
  rig.ctx.respond(0, {200, online("08:58", "19:58"), ""});  // one hour off: a wrong utc_offset_seconds
  CHECK_FALSE(rig.ctx.state.contains("verified_on"));
  CHECK_FALSE(rig.ctx.state.contains("correction"));
  CHECK(rig.ctx.invalidations == inv);
  auto after = draw(*rig.m, r);
  CHECK(same_rows(*shown, *after, 0, 112));
  CHECK(dot(*after) == 0);
  rig.loop.advance(19h);  // 23:00 local
  CHECK(rig.ctx.requests.size() == 1);
  rig.loop.advance(90min);  // 00:30 the next day
  REQUIRE(rig.ctx.requests.size() == 2);
  rig.ctx.respond(1, {200, online("07:57", "18:58"), ""});  // the next day's answer is usable
  CHECK(rig.ctx.state["verified_on"] == "2026-10-12");
}

TEST_CASE("a sun key with the default settings shows City? and asks nothing") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.start({{"city", ""}});
  CHECK(rig.ctx.requests.empty());
  const auto hint = fixtures::element_of(*rig.m, "city_hint");
  REQUIRE(hint);
  CHECK(hint->text == L"City?");
}

TEST_CASE("an unreadable geocoding answer is retried like a network error, not taken as an unknown city") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.start();
  REQUIRE(rig.ctx.requests.size() == 1);
  rig.ctx.respond(0, {200, "<html>Hotel Wi-Fi login</html>", ""});  // a captive portal
  CHECK_FALSE(fixtures::element_of(*rig.m, "city_hint"));
  rig.loop.advance(601s);
  REQUIRE(rig.ctx.requests.size() == 2);
  CHECK(rig.ctx.requests[1].first == ld::geocode_url("Brussels"));
  rig.ctx.respond(1, {200, R"({"error":true,"reason":"busy"})", ""});
  CHECK_FALSE(fixtures::element_of(*rig.m, "city_hint"));
  rig.loop.advance(601s);
  REQUIRE(rig.ctx.requests.size() == 3);
  rig.ctx.respond(2, {200, kGeo, ""});
  CHECK(rig.ctx.state["city"] == "Brussels");
  CHECK(fixtures::element_of(*rig.m, "line1"));
}

TEST_CASE("sun: the next event is normal, the other one idle; a status for City? and the polar cases") {
  SunRig rig(at_utc(kDay, 2, 0));
  rig.ctx.state = cached("2026-10-11");
  rig.start();
  CHECK(fixtures::element_of(*rig.m, "line1")->state == ld::ElementState::Normal);  // sunrise next
  CHECK(fixtures::element_of(*rig.m, "line2")->state == ld::ElementState::Idle);
  CHECK(fixtures::element_of(*rig.m, "symbol1")->text == L"↑");
  CHECK(fixtures::element_of(*rig.m, "line1")->text.find(L'↑') == std::wstring::npos);
  CHECK_FALSE(fixtures::element_of(*rig.m, "status"));
  SunRig lost(at_utc(kDay, 2, 0));
  lost.start({{"city", "Nowhereville"}});
  lost.ctx.respond(0, {200, R"({"results":[]})", ""});
  const auto st = fixtures::element_of(*lost.m, "city_hint");
  REQUIRE(st);
  CHECK(st->text == L"City?");
  CHECK(st->state == ld::ElementState::Idle);
  CHECK_FALSE(fixtures::element_of(*lost.m, "status"));
  const auto info = ld::sun_module_info();  // same look as weather's City?: 17 px, weight 600
  const auto spec = std::find_if(info.elements.begin(), info.elements.end(),
                                 [](const ld::ElementSpec& e) { return std::string_view(e.key) == "city_hint"; });
  REQUIRE(spec != info.elements.end());
  CHECK(spec->size == 17);
  CHECK(spec->weight == 600);
}
