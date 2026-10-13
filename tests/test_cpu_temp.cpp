#include <doctest/doctest.h>
#include "fake_context.h"
#include "module_fixtures.h"
#include "modules/common/format.h"
#include "modules/cpu_temp/cpu_temp.h"
#include "modules/schema.h"

using namespace std::chrono_literals;

static nlohmann::json settings() {
  nlohmann::json out;
  REQUIRE_FALSE(ld::apply_schema(ld::cpu_temp_module_info().settings_schema, nlohmann::json::object(), out));
  return out;
}

TEST_CASE("cpu_temp shows a dash and never invalidates without data") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = ld::make_cpu_temp_module([] { return std::optional<double>{}; });
  m->start(ctx, settings());
  CHECK(render_lit_pixels(*m, r) > 5);  // label + dash
  loop.advance(5000ms);
  CHECK(ctx.invalidations == 0);
}

TEST_CASE("cpu_temp renders the value and a graph, refreshed every second") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  double t = 52.4;
  auto m = ld::make_cpu_temp_module([&] { return std::optional<double>{t}; });
  m->start(ctx, settings());
  int base = ctx.invalidations;
  loop.advance(3000ms);
  CHECK(ctx.invalidations - base == 3);
  CHECK(render_lit_pixels(*m, r) > 20);
}

TEST_CASE("cpu_temp drops to a dash once when data disappears, then stays quiet") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  bool ok = true;
  auto m = ld::make_cpu_temp_module([&] { return ok ? std::optional<double>{60.0} : std::nullopt; });
  m->start(ctx, settings());
  loop.advance(3000ms);
  int with_data = render_lit_pixels(*m, r);
  ok = false;
  int before = ctx.invalidations;
  loop.advance(1000ms);
  CHECK(ctx.invalidations == before + 1);
  loop.advance(5000ms);
  CHECK(ctx.invalidations == before + 1);
  CHECK(render_lit_pixels(*m, r) < with_data);  // the graph is gone
  ok = true;
  loop.advance(1000ms);
  CHECK(ctx.invalidations == before + 2);
}

TEST_CASE("cpu_temp is a key-only module with thresholds, a gradient and three elements") {
  auto info = ld::cpu_temp_module_info();
  CHECK(info.id == "cpu_temp");
  CHECK(info.name == "CPU temperature");
  CHECK(info.allows(ld::SlotKind::Key));
  CHECK_FALSE(info.allows(ld::SlotKind::Knob));
  CHECK_FALSE(settings().contains("color"));
  CHECK(settings()["warn"] == 80.0);
  CHECK(settings()["crit"] == 90.0);
  REQUIRE(info.elements.size() == 3);
  CHECK(std::string(info.elements[0].key) == "label");
  CHECK(std::string(info.elements[1].key) == "value");
  CHECK(std::string(info.elements[2].key) == "graph");
}

TEST_CASE("cpu_temp: the value carries its threshold state and its metric; a dash is stale") {
  FakeLoop loop;
  FakeContext ctx(loop);
  std::optional<double> t = 92.0;
  auto m = ld::make_cpu_temp_module([&] { return t; });
  m->start(ctx, settings());
  auto v = fixtures::element_of(*m, "value");
  REQUIRE(v);
  CHECK(v->state == ld::ElementState::Critical);
  CHECK(v->text == L"92°C");
  CHECK(*v->metric == 92.0);
  t.reset();
  loop.advance(1000ms);
  v = fixtures::element_of(*m, "value");
  CHECK(v->state == ld::ElementState::Stale);
  CHECK(v->text == ld::kDash);
  CHECK_FALSE(fixtures::element_of(*m, "graph"));
}

TEST_CASE("service_temperature_reader never throws (machine dependent result)") {
  auto read = ld::service_temperature_reader();
  CHECK_NOTHROW(read());
  CHECK_NOTHROW(read());
  auto tagged = ld::service_temperature_reader("cpu_load");
  CHECK_NOTHROW(tagged());
}
