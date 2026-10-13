// tests/test_ping.cpp
#include <doctest/doctest.h>
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/format.h"
#include "modules/ping/ping.h"

using namespace std::chrono_literals;

namespace {
// Fake resolver and echo, moved by the test. Jobs run inline, or are held until released.
struct Net {
  std::optional<std::string> address = "1.1.1.1";
  std::optional<int> rtt = 12;
  int resolves = 0, echoes = 0;
  std::string last_host, last_address;
  int last_timeout = 0;
  bool hold = false;
  std::vector<std::function<void()>> held;
  ld::PingResolver resolver() {
    return [this](const std::string& host) {
      ++resolves;
      last_host = host;
      return address;
    };
  }
  ld::PingEcho echo() {
    return [this](const std::string& a, int timeout_ms) {
      ++echoes;
      last_address = a;
      last_timeout = timeout_ms;
      return rtt;
    };
  }
  ld::Runner runner() {
    return [this](std::function<void()> job) {
      if (hold) held.push_back(std::move(job));
      else job();
    };
  }
};
std::unique_ptr<fixtures::Rig> start(Net& n, nlohmann::json over = nlohmann::json::object()) {
  auto rig = std::make_unique<fixtures::Rig>();
  rig->start(ld::ping_module_info(), ld::make_ping_module(n.resolver(), n.echo(), n.runner()), over);
  return rig;
}
std::wstring text(fixtures::Rig& rig, const char* key) {
  auto e = fixtures::element_of(*rig.m, key);
  return e ? e->text : L"<none>";
}
ld::ElementState state(fixtures::Rig& rig, const char* key) { return fixtures::element_of(*rig.m, key)->state; }
}  // namespace

TEST_CASE("format_latency") {
  CHECK(ld::format_latency(12) == L"12 ms");
  CHECK(ld::format_latency(0) == L"<1 ms");
  CHECK(ld::format_latency(1234) == L"1234 ms");
}

TEST_CASE("ping: schema defaults and validation") {
  const auto info = ld::ping_module_info();
  CHECK(ld::element_spec_errors(info.elements).empty());
  nlohmann::json s;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  CHECK(s["host"] == "1.1.1.1");
  CHECK(s["interval_s"] == 2);
  CHECK(s["timeout_ms"] == 1000);
  CHECK(s["warn_ms"] == 80);
  CHECK(ld::apply_schema(info.settings_schema, {{"interval_s", 0}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"interval_s", 61}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"timeout_ms", 100}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"timeout_ms", 5001}}, s).has_value());
}

TEST_CASE("ping: the latency in the middle, the target at the top right, the loss and a graph at the bottom") {
  Net n;
  auto rig = start(n);
  CHECK(text(*rig, "label") == L"PING");
  CHECK(text(*rig, "target") == L"1.1.1.1");
  CHECK(text(*rig, "value") == L"12 ms");
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
  CHECK(text(*rig, "loss") == L"loss 0 %");
  CHECK(n.last_host == "1.1.1.1");
  CHECK(n.last_timeout == 1000);
  rig->loop.advance(2000ms);
  const auto graph = fixtures::element_of(*rig->m, "graph");
  REQUIRE(graph);
  CHECK(graph->capacity == 30);  // 60 s at one probe every 2 s
  CHECK(graph->points.size() == 2);
  CHECK(graph->points[1] == doctest::Approx(12.0 / 80.0));  // scaled to the warning level
}

TEST_CASE("ping: a dash in the stale state until the first answer") {
  Net n;
  n.hold = true;
  auto rig = start(n);
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  CHECK(text(*rig, "loss") == L"<none>");
}

TEST_CASE("ping: warning above warn_ms with hysteresis, critical on a timeout") {
  Net n;
  auto rig = start(n, {{"warn_ms", 50}, {"hysteresis", 10}, {"interval_s", 1}});
  n.rtt = 60;
  rig->loop.advance(1000ms);
  CHECK(state(*rig, "value") == ld::ElementState::Warning);
  n.rtt = 45;  // below the threshold, not below threshold - hysteresis
  rig->loop.advance(1000ms);
  CHECK(state(*rig, "value") == ld::ElementState::Warning);
  n.rtt = 39;
  rig->loop.advance(1000ms);
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
  n.rtt = std::nullopt;  // timed out
  rig->loop.advance(1000ms);
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Critical);
  n.rtt = 20;
  rig->loop.advance(1000ms);
  CHECK(text(*rig, "value") == L"20 ms");
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
}

TEST_CASE("ping: the loss over the last 20 probes") {
  Net n;
  auto rig = start(n, {{"interval_s", 1}});  // 1 answered
  n.rtt = std::nullopt;
  rig->loop.advance(1000ms);  // 1 lost of 2
  CHECK(text(*rig, "loss") == L"loss 50 %");
  n.rtt = 10;
  rig->loop.advance(18000ms);  // 1 lost of 20
  CHECK(text(*rig, "loss") == L"loss 5 %");
  rig->loop.advance(2000ms);  // the first two leave the window, the lost one with them
  CHECK(text(*rig, "loss") == L"loss 0 %");
}

TEST_CASE("ping: one probe in flight at a time") {
  Net n;
  n.hold = true;
  auto rig = start(n, {{"interval_s", 1}, {"timeout_ms", 5000}});
  rig->loop.advance(3000ms);
  CHECK(n.held.size() == 1);  // the next ticks found a probe in flight
  n.held[0]();
  rig->loop.drain();
  CHECK(text(*rig, "value") == L"12 ms");
  rig->loop.advance(1000ms);
  CHECK(n.held.size() == 2);
}

TEST_CASE("ping: the name is resolved once and cached for five minutes; gateway for one") {
  Net n;
  auto rig = start(n, {{"host", "example.com"}, {"interval_s", 10}});
  CHECK(n.resolves == 1);
  CHECK(n.last_host == "example.com");
  CHECK(n.last_address == "1.1.1.1");
  rig->loop.advance(290s);
  CHECK(n.resolves == 1);
  rig->loop.advance(20s);
  CHECK(n.resolves == 2);

  Net g;
  g.address = "192.168.1.1";
  auto gw = start(g, {{"host", "gateway"}, {"interval_s", 10}});
  CHECK(g.last_host == "gateway");
  CHECK(text(*gw, "target") == L"gateway");
  gw->loop.advance(60s);
  CHECK(g.resolves == 2);
}

TEST_CASE("ping: a name that does not resolve shows a stale dash and probes nothing") {
  Net n;
  n.address = std::nullopt;
  auto rig = start(n, {{"host", "nowhere.invalid"}});
  CHECK(n.echoes == 0);
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  rig->loop.advance(2000ms);
  CHECK(n.resolves == 2);  // tried again at the next probe
  n.address = "10.0.0.1";
  rig->loop.advance(2000ms);
  CHECK(text(*rig, "value") == L"12 ms");
}

TEST_CASE("ping: an empty host asks for one") {
  Net n;
  auto rig = start(n, {{"host", "  "}});
  CHECK(text(*rig, "value") == L"Host?");
  CHECK(state(*rig, "value") == ld::ElementState::Idle);
  rig->loop.advance(10s);
  CHECK(n.resolves == 0);
}

TEST_CASE("golden: ping") {
  Net n;
  auto rig = start(n, {{"interval_s", 2}});
  const int rtts[] = {14, 12, 13, 18, 12, 11, 12, 25, 13, 12, 12, 14, 13, 12, 16, 12, 11, 12, 13, 12,
                      12, 15, 13, 12, 22, 12, 13, 12, 11};
  for (int r : rtts) {
    n.rtt = r;
    rig->loop.advance(2000ms);
  }
  ld::Renderer r;
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "ping");
}

TEST_CASE("net: the real resolver and ICMP echo reach 1.1.1.1 and the gateway" * doctest::skip()) {
  CHECK(ld::resolve_ping_host("one.one.one.one"));
  const auto gw = ld::resolve_ping_host("gateway");
  REQUIRE(gw);
  MESSAGE("gateway " << *gw);
  CHECK(ld::icmp_echo("1.1.1.1", 1000));
  CHECK(ld::icmp_echo(*gw, 1000));
}
