#include <doctest/doctest.h>
#include "fake_context.h"
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/format.h"
#include "modules/common/state_settings.h"
#include "modules/common/temp_header.h"
#include "modules/cpu_load/cpu_load.h"
#include "modules/schema.h"

using namespace std::chrono_literals;

namespace {
// A cpu_load key with an injected temperature; counts the reads.
struct CpuRig {
  FakeLoop loop;
  FakeContext ctx{loop};
  std::unique_ptr<ld::Module> m;
  int reads = 0;
  std::optional<double> temp;
  CpuRig(bool show_temperature, std::optional<double> t) : temp(t) {
    nlohmann::json s;
    REQUIRE_FALSE(ld::apply_schema(ld::cpu_load_module_info().settings_schema,
                                   nlohmann::json{{"show_temperature", show_temperature}}, s));
    m = ld::make_cpu_load_module([this] {
      ++reads;
      return temp;
    });
    m->start(ctx, s);
  }
};
// The right half of the header band, above the percentage: only the temperature draws there.
int corner_ink(ld::Surface& s) { return ink(s, 76, 0, 112, 18).count; }
}  // namespace

TEST_CASE("format_temperature: rounded degrees with a narrow no-break space, or a dash") {
  CHECK(ld::format_temperature(52) == L"52 °C");
  CHECK(ld::format_temperature(-3) == L"-3 °C");
  CHECK(ld::format_temperature(std::nullopt) == std::wstring(ld::kDash));
}

TEST_CASE("cpu_load never reads the temperature unless show_temperature is set") {
  CpuRig rig(false, 50.0);
  rig.loop.advance(3000ms);
  CHECK(rig.reads == 0);
  nlohmann::json defaults;
  REQUIRE_FALSE(ld::apply_schema(ld::cpu_load_module_info().settings_schema, nlohmann::json::object(), defaults));
  CHECK(defaults["show_temperature"] == false);  // existing configs keep their look
}

TEST_CASE("cpu_load with show_temperature draws the temperature at the top right, a dash without data") {
  ld::Renderer r;
  CpuRig plain(false, std::nullopt), hot(true, 52.4), none(true, std::nullopt);
  CHECK(hot.reads == 1);  // read at start: right from the first image
  auto p = draw(*plain.m, r);
  auto h = draw(*hot.m, r);
  auto n = draw(*none.m, r);
  CHECK(corner_ink(*p) < corner_ink(*n));  // the dash draws something...
  CHECK(corner_ink(*n) < corner_ink(*h));  // ..."52 °C" draws more
  hot.loop.advance(2000ms);
  CHECK(hot.reads == 3);  // then once per sample
}

TEST_CASE("cpu_load declares its elements: value, label, temperature, graph") {
  const auto info = ld::cpu_load_module_info();
  REQUIRE(info.elements.size() == 4);
  CHECK(std::string(info.elements[2].key) == "temp");
  CHECK(info.elements[2].size == ld::kTempElement.size);
  CHECK(info.elements[3].kind == ld::ElementKind::Graph);
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  CHECK_FALSE(s.contains("color"));  // moved to elements.value.color (v1 config migration)
  CHECK(s["warn"] == 80.0);
  CHECK(s["crit"] == 90.0);
  CHECK(s["hysteresis"] == 3.0);
  CHECK(s["cold_at"] == 40.0);
}

TEST_CASE("cpu_load: the temperature warns from 80 °C, is critical from 90 °C, with 3 °C of hysteresis") {
  CpuRig rig(true, 79.0);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Normal);
  rig.temp = 80.0;
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Warning);
  rig.temp = 91.0;
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Critical);
  rig.temp = 87.0;
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Critical);
  rig.temp = 76.0;
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Normal);
  const auto temp = fixtures::element_of(*rig.m, "temp");
  REQUIRE(temp->metric);
  CHECK(*temp->metric == 76.0);
  CHECK(temp->gradient == ld::kTemperatureGradient);
  rig.temp.reset();
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Stale);
  CHECK_FALSE(fixtures::element_of(*rig.m, "temp")->metric);
}

TEST_CASE("cpu_load takes the state from the raw temperature, like cpu_temp: 79.6 °C is not yet a warning") {
  CpuRig rig(true, 79.6);
  const auto temp = fixtures::element_of(*rig.m, "temp");
  CHECK(temp->text == ld::format_temperature(80));  // shown rounded
  CHECK(temp->state == ld::ElementState::Normal);
}

TEST_CASE("cpu_load: a temperature gap resets the state, like cpu_temp") {
  CpuRig rig(true, 91.0);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Critical);
  rig.temp.reset();
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Stale);
  rig.temp = 88.0;  // above crit - hysteresis: would stay Critical without the reset
  rig.loop.advance(1000ms);
  CHECK(fixtures::element_of(*rig.m, "temp")->state == ld::ElementState::Warning);
}

TEST_CASE("cpu_load without show_temperature shows no temperature element") {
  CpuRig rig(false, 50.0);
  CHECK_FALSE(fixtures::element_of(*rig.m, "temp"));
  CHECK(fixtures::element_of(*rig.m, "label")->text == L"CPU");
}
