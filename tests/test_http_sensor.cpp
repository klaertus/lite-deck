// tests/test_http_sensor.cpp
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "core/log.h"
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/format.h"
#include "modules/http_sensor/http_sensor.h"

using namespace std::chrono_literals;
using nlohmann::json;

TEST_CASE("parse_json_path: dotted keys and indices") {
  auto p = ld::parse_json_path("result.temperature");
  REQUIRE(p);
  REQUIRE(p->size() == 2);
  CHECK((*p)[0].key == "result");
  CHECK((*p)[1].key == "temperature");
  p = ld::parse_json_path("items[0].value");
  REQUIRE(p);
  REQUIRE(p->size() == 3);
  CHECK((*p)[1].index == 0);
  CHECK((*p)[1].key.empty());
  p = ld::parse_json_path("[2][1]");
  REQUIRE(p);
  CHECK(p->size() == 2);
  CHECK(ld::parse_json_path("")->empty());  // the whole answer
  CHECK(ld::parse_json_path("$.a")->size() == 1);
  CHECK_FALSE(ld::parse_json_path("a..b"));
  CHECK_FALSE(ld::parse_json_path("a["));
  CHECK_FALSE(ld::parse_json_path("a[x]"));
  CHECK_FALSE(ld::parse_json_path("a[-1]"));
  CHECK_FALSE(ld::parse_json_path("a.b]"));
}

TEST_CASE("json_at follows the path, or gives nothing") {
  const json j = json::parse(R"({"result":{"temperature":21.5},"items":[{"value":"on"},{"value":3}]})");
  CHECK(*ld::json_at(j, *ld::parse_json_path("result.temperature")) == 21.5);
  CHECK(*ld::json_at(j, *ld::parse_json_path("items[1].value")) == 3);
  CHECK_FALSE(ld::json_at(j, *ld::parse_json_path("items[2].value")));
  CHECK_FALSE(ld::json_at(j, *ld::parse_json_path("result.humidity")));
  CHECK_FALSE(ld::json_at(j, *ld::parse_json_path("result[0]")));
  CHECK_FALSE(ld::json_at(j, *ld::parse_json_path("items.value")));
}

TEST_CASE("http_sensor: schema defaults, a masked header value, and validation") {
  const auto info = ld::http_sensor_module_info();
  CHECK(ld::element_spec_errors(info.elements).empty());
  json s;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, json::object(), s));
  CHECK(s["url"] == "");
  CHECK(s["label"] == "SENSOR");
  CHECK(s["decimals"] == 1);
  CHECK(s["interval_s"] == 30);
  CHECK(s["direction"] == "off");
  bool secret = false;
  for (const json& f : info.settings_schema["fields"])
    if (f["key"] == "header_value") secret = f["type"] == "secret";
  CHECK(secret);
  CHECK(ld::apply_schema(info.settings_schema, {{"interval_s", 4}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"decimals", 4}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"direction", "sideways"}}, s).has_value());
}

namespace {
std::unique_ptr<fixtures::Rig> start(json over) {
  auto rig = std::make_unique<fixtures::Rig>();
  rig->start(ld::http_sensor_module_info(), ld::http_sensor_module_info().create(), over);
  return rig;
}
std::wstring text(fixtures::Rig& rig, const char* key) {
  auto e = fixtures::element_of(*rig.m, key);
  return e ? e->text : L"<none>";
}
ld::ElementState state(fixtures::Rig& rig, const char* key) { return fixtures::element_of(*rig.m, key)->state; }
void answer(fixtures::Rig& rig, std::string body, int status = 200) {
  rig.ctx.respond(rig.ctx.requests.size() - 1, ld::HttpResponse{status, std::move(body), ""});
}
const json kHa{{"url", "http://ha.local:8123/api/states/sensor.office?x=1"},
               {"json_path", "attributes.temperature"},
               {"label", "OFFICE"},
               {"unit", "°C"},
               {"header_name", "Authorization"},
               {"header_value", "Bearer s3cret"}};
}  // namespace

TEST_CASE("http_sensor: asks the url with its header, then shows the value, its label and its unit") {
  auto rig = start(kHa);
  REQUIRE(rig->ctx.requests.size() == 1);
  const ld::HttpRequest req = rig->ctx.sent.at(0);
  CHECK(req.url == "http://ha.local:8123/api/states/sensor.office?x=1");
  REQUIRE(req.header);
  CHECK(req.header->first == "Authorization");
  CHECK(req.header->second == "Bearer s3cret");
  CHECK(req.allow_plain_http);
  CHECK(req.lane == ld::HttpLane::Isolated);  // a slow sensor never delays the other modules
  CHECK(text(*rig, "value") == ld::kDash);  // waiting
  CHECK(state(*rig, "value") == ld::ElementState::Idle);
  answer(*rig, R"({"attributes":{"temperature":21.46}})");
  CHECK(text(*rig, "label") == L"OFFICE");
  CHECK(text(*rig, "value") == L"21.5");
  CHECK(text(*rig, "unit") == L"°C");
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
}

TEST_CASE("http_sensor: https needs no opt-in, and an empty header name sends none") {
  auto rig = start({{"url", "https://api.example.com/v1"}, {"header_value", "ignored"}});
  const ld::HttpRequest req = rig->ctx.sent.at(0);
  CHECK(req.url == "https://api.example.com/v1");
  CHECK_FALSE(req.header);
  CHECK_FALSE(req.allow_plain_http);
  CHECK(req.lane == ld::HttpLane::Isolated);
  answer(*rig, "42");  // the whole answer
  CHECK(text(*rig, "value") == L"42.0");
  CHECK(text(*rig, "label") == L"SENSOR");
  CHECK(text(*rig, "unit") == L"<none>");
}

TEST_CASE("http_sensor: decimals, numeric strings, text and booleans") {
  auto rig = start({{"url", "https://h/x"}, {"json_path", "v"}, {"decimals", 0}});
  answer(*rig, R"({"v":21.6})");
  CHECK(text(*rig, "value") == L"22");
  rig->loop.advance(30s);
  answer(*rig, R"({"v":" 18.25 "})");  // Home Assistant states are strings
  CHECK(text(*rig, "value") == L"18");
  rig->loop.advance(30s);
  answer(*rig, R"({"v":"unavailable"})");
  CHECK(text(*rig, "value") == L"unavailable");
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
  rig->loop.advance(30s);
  answer(*rig, R"({"v":true})");
  CHECK(text(*rig, "value") == L"true");
  rig->loop.advance(30s);
  answer(*rig, R"({"v":"a text far too long for any key, which the module cuts before the engine clips it"})");
  CHECK(text(*rig, "value").size() <= 24);
  CHECK(text(*rig, "value").back() == L'…');
}

TEST_CASE("http_sensor: a missing path shows ? in the idle state") {
  auto rig = start({{"url", "https://h/x"}, {"json_path", "a.b"}});
  answer(*rig, R"({"a":{}})");
  CHECK(text(*rig, "value") == L"?");
  CHECK(state(*rig, "value") == ld::ElementState::Idle);
  CHECK(text(*rig, "unit") == L"<none>");
  rig->loop.advance(30s);
  CHECK(rig->ctx.requests.size() == 2);  // not a failure: the normal period
}

TEST_CASE("http_sensor: a failure greys the last value; without one, a stale dash") {
  auto rig = start({{"url", "https://h/x"}, {"json_path", "v"}, {"unit", "W"}});
  answer(*rig, "", 503);
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  rig->loop.advance(60s);
  answer(*rig, R"({"v":250})");
  CHECK(text(*rig, "value") == L"250.0");
  rig->loop.advance(30s);
  answer(*rig, "<html>not json</html>");
  CHECK(text(*rig, "value") == L"250.0");
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  CHECK(state(*rig, "unit") == ld::ElementState::Stale);
}

TEST_CASE("http_sensor: after failures the wait doubles, never below interval_s, back to it on success") {
  auto rig = start({{"url", "https://h/x"}, {"interval_s", 10}});
  auto& reqs = rig->ctx.requests;
  answer(*rig, "", 500);  // 1st failure: next in 20 s
  rig->loop.advance(19s);
  CHECK(reqs.size() == 1);
  rig->loop.advance(1s);
  CHECK(reqs.size() == 2);
  rig->ctx.respond(1, ld::HttpResponse{0, "", "timeout"});  // 2nd: 40 s
  rig->loop.advance(39s);
  CHECK(reqs.size() == 2);
  rig->loop.advance(1s);
  CHECK(reqs.size() == 3);
  answer(*rig, "1");  // success: back to 10 s
  rig->loop.advance(10s);
  CHECK(reqs.size() == 4);
  answer(*rig, "1");
  for (int i = 0; i < 12; ++i) {  // a long outage: capped at 10 minutes
    rig->loop.advance(std::chrono::hours(1));
    answer(*rig, "", 500);
  }
  const std::size_t before = reqs.size();
  rig->loop.advance(600s);
  CHECK(reqs.size() == before + 1);
}

TEST_CASE("http_sensor: no request while one is in flight") {
  auto rig = start({{"url", "https://h/x"}, {"interval_s", 5}});
  rig->loop.advance(60s);
  CHECK(rig->ctx.requests.size() == 1);
}

TEST_CASE("http_sensor: thresholds above or below, with hysteresis") {
  auto above = start({{"url", "https://h/x"}, {"direction", "above"}, {"warn", 30}, {"crit", 40}, {"hysteresis", 2}});
  answer(*above, "31");
  CHECK(state(*above, "value") == ld::ElementState::Warning);
  above->loop.advance(30s);
  answer(*above, "41");
  CHECK(state(*above, "value") == ld::ElementState::Critical);
  above->loop.advance(30s);
  answer(*above, "39");
  CHECK(state(*above, "value") == ld::ElementState::Critical);
  above->loop.advance(30s);
  answer(*above, "27");
  CHECK(state(*above, "value") == ld::ElementState::Normal);

  auto below = start({{"url", "https://h/x"}, {"direction", "below"}, {"warn", 20}, {"crit", 10}, {"hysteresis", 1}});
  answer(*below, "15");
  CHECK(state(*below, "value") == ld::ElementState::Warning);
  below->loop.advance(30s);
  answer(*below, "5");
  CHECK(state(*below, "value") == ld::ElementState::Critical);
  below->loop.advance(30s);
  answer(*below, "25");
  CHECK(state(*below, "value") == ld::ElementState::Normal);

  auto off = start({{"url", "https://h/x"}, {"warn", 1}, {"crit", 2}});
  answer(*off, "100");
  CHECK(state(*off, "value") == ld::ElementState::Normal);
}

TEST_CASE("http_sensor: an empty or unusable setting asks for it and sends nothing") {
  auto none = start(json::object());
  CHECK(text(*none, "value") == L"URL?");
  CHECK(state(*none, "value") == ld::ElementState::Idle);
  auto ftp = start({{"url", "ftp://h/x"}});
  CHECK(text(*ftp, "value") == L"URL?");
  auto spaces = start({{"url", "https://h/a b"}});
  CHECK(text(*spaces, "value") == L"URL?");
  auto path = start({{"url", "https://h/x"}, {"json_path", "a[x]"}});
  CHECK(text(*path, "value") == L"Path?");
  auto header = start({{"url", "https://h/x"}, {"header_name", "Bad Name"}});
  CHECK(text(*header, "value") == L"Header?");
  auto injected = start({{"url", "https://h/x"}, {"header_name", "X"}, {"header_value", "a\r\nB: c"}});
  CHECK(text(*injected, "value") == L"Header?");
  for (auto* r : {none.get(), ftp.get(), spaces.get(), path.get(), header.get(), injected.get()})
    CHECK(r->ctx.requests.empty());
}

TEST_CASE("http_sensor: the log names the host and path only, never the query or the header") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_http_sensor_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    auto rig = start(kHa);
    answer(*rig, "", 401);
    rig->loop.advance(60s);
    answer(*rig, "not json");
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("http://ha.local:8123/api/states/sensor.office") != std::string::npos);
  CHECK(log.find("x=1") == std::string::npos);
  CHECK(log.find("s3cret") == std::string::npos);
  CHECK(log.find("Bearer") == std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("golden: http_sensor") {
  ld::Renderer r;
  auto rig = start(kHa);
  answer(*rig, R"({"attributes":{"temperature":21.46}})");
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "http_sensor");
}
