// tests/test_net_speed.cpp
#include <doctest/doctest.h>
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/format.h"
#include "modules/net_speed/net_speed.h"

using namespace std::chrono_literals;
using ld::NetCounter;

TEST_CASE("format_rate: B/KB/MB/GB per second, one decimal below 100, bytes or bits") {
  using R = ld::RateText;
  auto check = [](double v, bool bits, std::wstring n, std::wstring u) {
    const R r = ld::format_rate(v, bits);
    CHECK(r.number == n);
    CHECK(r.unit == u);
  };
  check(0, false, L"0", L"B/s");
  check(512.4, false, L"512", L"B/s");
  check(999.4, false, L"999", L"B/s");
  check(999.6, false, L"1.0", L"KB/s");
  check(2'100'000, false, L"2.1", L"MB/s");
  check(12'440'000, false, L"12.4", L"MB/s");
  check(123'400'000, false, L"123", L"MB/s");
  check(999'700'000, false, L"1.0", L"GB/s");
  check(3.5e12, false, L"3500", L"GB/s");  // no unit above GB/s
  check(2'100'000, true, L"16.8", L"Mb/s");
  check(100, true, L"800", L"b/s");
  check(-5, false, L"0", L"B/s");  // never negative
}

TEST_CASE("net_delta: sums the interfaces present twice; a counter going back skips the sample") {
  const std::vector<NetCounter> a{{1, 1000, 100}, {2, 5000, 50}};
  const std::vector<NetCounter> b{{1, 3000, 400}, {2, 5500, 50}, {3, 999999, 999999}};  // 3 is new: ignored
  auto d = ld::net_delta(a, b);
  REQUIRE(d);
  CHECK(d->rx == 2500);
  CHECK(d->tx == 300);
  const std::vector<NetCounter> gone{{2, 6000, 60}};  // 1 went away: the others still count
  d = ld::net_delta(b, gone);
  REQUIRE(d);
  CHECK(d->rx == 500);
  const std::vector<NetCounter> wrapped{{1, 10, 400}, {2, 5600, 50}};  // 1 wrapped or was reset
  CHECK_FALSE(ld::net_delta(b, wrapped));
}

TEST_CASE("net_interface_selected: auto keeps the physical interfaces that are up; a name picks one") {
  ld::NetInterfaceInfo eth{.alias = L"Ethernet", .description = L"Intel(R) Ethernet Controller I225-V",
                           .hardware = true, .up = true};
  CHECK(ld::net_interface_selected(eth, "auto"));
  CHECK(ld::net_interface_selected(eth, "ethernet"));  // the alias, any case
  CHECK(ld::net_interface_selected(eth, "Intel(R) Ethernet Controller I225-V"));
  CHECK_FALSE(ld::net_interface_selected(eth, "Wi-Fi"));
  auto down = eth;
  down.up = false;
  CHECK_FALSE(ld::net_interface_selected(down, "auto"));
  auto loopback = eth;
  loopback.loopback = true;
  CHECK_FALSE(ld::net_interface_selected(loopback, "auto"));
  auto tunnel = eth;
  tunnel.tunnel = true;
  CHECK_FALSE(ld::net_interface_selected(tunnel, "auto"));
  auto filter = eth;
  filter.filter = true;  // the WFP / QoS filter rows that repeat a physical adapter
  CHECK_FALSE(ld::net_interface_selected(filter, "auto"));
  auto hyperv = eth;
  hyperv.hardware = false;
  hyperv.alias = L"vEthernet (WSL)";
  hyperv.description = L"Hyper-V Virtual Ethernet Adapter";
  CHECK_FALSE(ld::net_interface_selected(hyperv, "auto"));
  CHECK(ld::net_interface_selected(hyperv, "vEthernet (WSL)"));  // named: taken even if virtual
  auto vbox = eth;  // some virtual adapters claim to be hardware
  vbox.description = L"VirtualBox Host-Only Ethernet Adapter";
  CHECK_FALSE(ld::net_interface_selected(vbox, "auto"));
}

namespace {
// A fake interface table moved by the test: one interface.
struct Counters {
  std::uint64_t rx = 0, tx = 0;
  bool fail = false;
  ld::NetReader reader() {
    return [this]() -> std::optional<std::vector<NetCounter>> {
      if (fail) return std::nullopt;
      return std::vector<NetCounter>{{7, rx, tx}};
    };
  }
};
std::unique_ptr<fixtures::Rig> start(Counters& c, nlohmann::json over = nlohmann::json::object()) {
  auto rig = std::make_unique<fixtures::Rig>();
  rig->start(ld::net_speed_module_info(), ld::make_net_speed_module(c.reader()), over);
  return rig;
}
}  // namespace

TEST_CASE("net_speed: schema defaults and validation") {
  const auto info = ld::net_speed_module_info();
  CHECK(ld::element_spec_errors(info.elements).empty());
  nlohmann::json s;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  CHECK(s["unit"] == "bytes");
  CHECK(s["interface"] == "auto");
  CHECK(ld::apply_schema(info.settings_schema, {{"unit", "nibbles"}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"interface", 3}}, s).has_value());
}

TEST_CASE("net_speed: download in the middle, upload at the top right, a 60 s graph of the download") {
  Counters c;
  auto rig = start(c);
  auto value = fixtures::element_of(*rig->m, "value");
  REQUIRE(value);
  CHECK(value->text == ld::kDash);  // no rate before a second reading
  c.rx += 2'100'000;
  c.tx += 512;
  rig->loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig->m, "label")->text == L"NET");
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"2.1");
  CHECK(fixtures::element_of(*rig->m, "unit")->text == L"MB/s");
  CHECK(fixtures::element_of(*rig->m, "up")->text == L"↑ 512 B/s");
  c.rx += 1'000'000;
  rig->loop.advance(1000ms);
  const auto graph = fixtures::element_of(*rig->m, "graph");
  REQUIRE(graph);
  CHECK(graph->capacity == 60);
  REQUIRE(graph->points.size() == 2);
  CHECK(graph->points[0] == doctest::Approx(1.0));  // scaled to the largest value shown
  CHECK(graph->points[1] == doctest::Approx(1.0 / 2.1));
}

TEST_CASE("net_speed: the rate is per second of steady time, even when a tick comes late") {
  Counters c;
  auto rig = start(c);
  c.rx += 3'000'000;
  rig->loop.t += 2000ms;  // the loop was busy: one tick for two seconds
  rig->loop.advance(0ms);  // a catch-up tick at the same instant measures nothing
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"1.5");
}

TEST_CASE("net_speed: bits") {
  Counters c;
  auto rig = start(c, {{"unit", "bits"}});
  c.rx += 2'100'000;
  rig->loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"16.8");
  CHECK(fixtures::element_of(*rig->m, "unit")->text == L"Mb/s");
}

TEST_CASE("net_speed: a counter reset is skipped, never a negative or huge rate") {
  Counters c;
  c.rx = 50'000'000;
  auto rig = start(c);
  c.rx += 1'000'000;
  rig->loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"1.0");
  c.rx = 1000;  // reset (adapter restarted)
  rig->loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"1.0");  // the last rate stays
  CHECK(fixtures::element_of(*rig->m, "graph")->points.size() == 1);  // no point for the skipped second
  c.rx += 2000;
  rig->loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"2.0");
  CHECK(fixtures::element_of(*rig->m, "unit")->text == L"KB/s");
}

TEST_CASE("net_speed: a failed read shows a stale dash, then recovers") {
  Counters c;
  auto rig = start(c);
  c.rx += 1000;
  rig->loop.advance(1000ms);
  c.fail = true;
  rig->loop.advance(1000ms);
  auto value = fixtures::element_of(*rig->m, "value");
  CHECK(value->text == ld::kDash);
  CHECK(value->state == ld::ElementState::Stale);
  CHECK_FALSE(fixtures::element_of(*rig->m, "up"));
  c.fail = false;
  rig->loop.advance(1000ms);  // a new baseline
  c.rx += 4000;
  rig->loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig->m, "value")->text == L"4.0");
  CHECK(fixtures::element_of(*rig->m, "value")->state == ld::ElementState::Normal);
}

TEST_CASE("manual: the real interface table lists the adapters that are up" * doctest::skip()) {
  const auto all = ld::if_table_reader("auto")();
  REQUIRE(all);
  MESSAGE(all->size() << " interface(s) counted by auto");
  CHECK_FALSE(all->empty());
}

TEST_CASE("golden: net_speed") {
  Counters c;
  auto rig = start(c);
  // A minute of browsing (2 to 4 MB/s), then a download climbing to 15 MB/s, now at 12.4 MB/s.
  const std::uint64_t burst[] = {6'000'000, 11'000'000, 14'500'000, 15'000'000, 13'800'000, 12'400'000};
  for (int i = 0; i < 60; ++i) {
    const std::uint64_t d = i >= 54 ? burst[i - 54] : static_cast<std::uint64_t>(3'000'000 + 1'000'000 * std::sin(i / 4.0));
    c.rx += d;
    c.tx += 300'000;
    rig->loop.advance(1000ms);
  }
  ld::Renderer r;
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "net_speed");
}
