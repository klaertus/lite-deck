#include <doctest/doctest.h>
#include "fake_context.h"
#include "golden.h"
#include "modules/common/format.h"
#include "modules/gpu/gpu.h"
#include "module_fixtures.h"
#include "modules/schema.h"

using namespace std::chrono_literals;

static nlohmann::json settings(const char* mode) {
  nlohmann::json out;
  REQUIRE_FALSE(ld::apply_schema(ld::gpu_module_info().settings_schema, nlohmann::json{{"mode", mode}}, out));
  return out;
}

TEST_CASE("gpu shows a dash without a GPU reading") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; });
  CHECK_NOTHROW(m->start(ctx, settings("vram")));
  CHECK(render_lit_pixels(*m, r) > 5);  // label + dash
  loop.advance(3000ms);
  CHECK(ctx.invalidations == 0);  // the dash never changes
}

TEST_CASE("gpu temp mode invalidates only when the value changes") {
  FakeLoop loop;
  FakeContext ctx(loop);
  int t = 50;
  auto m = ld::make_gpu_module([&] { return std::optional<ld::GpuReading>{ld::GpuReading{4.0, t}}; });
  m->start(ctx, settings("temp"));
  int base = ctx.invalidations;
  loop.advance(3000ms);
  CHECK(ctx.invalidations == base);
  t = 55;
  loop.advance(1000ms);
  CHECK(ctx.invalidations == base + 1);
}

TEST_CASE("gpu_read never throws (machine dependent result)") {
  CHECK_NOTHROW(ld::gpu_read(0));
  CHECK_NOTHROW(ld::gpu_read(7));  // index past the last adapter -> nullopt, no crash
  CHECK_FALSE(ld::gpu_read(7).has_value());
}

TEST_CASE("manual: gpu_read reports the main GPU" * doctest::skip()) {
  auto r = ld::gpu_read(0, true);
  REQUIRE(r);
  MESSAGE("vram GiB = " << r->vram_used_gib << " of " << r->vram_total_gib << ", temp C = " << r->temp_c.value_or(-1));
  CHECK(r->vram_total_gib > 0.0);
  CHECK(r->vram_used_gib > 0.0);
  CHECK(r->temp_c.value_or(0) > 0);
}

TEST_CASE("gpu vram mode renders the value and invalidates once") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{ld::GpuReading{5.126, 50}}; });
  m->start(ctx, settings("vram"));
  CHECK(render_lit_pixels(*m, r) > 20);
  loop.advance(3000ms);
  CHECK(ctx.invalidations == 1);
}

static nlohmann::json usage_settings(bool show_temperature) {
  nlohmann::json out;
  REQUIRE_FALSE(ld::apply_schema(ld::gpu_module_info().settings_schema,
                                 nlohmann::json{{"mode", "usage"}, {"show_temperature", show_temperature}}, out));
  return out;
}

static std::unique_ptr<ld::Module> usage_module(ld::GpuUsageReader usage) {
  return ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; }, std::move(usage));
}

TEST_CASE("gpu usage mode renders the percentage and a graph, refreshed every second") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = usage_module([] { return ld::GpuUsageSample{37.0, 61}; });
  m->start(ctx, usage_settings(false));
  const int base = ctx.invalidations;
  loop.advance(3000ms);
  CHECK(ctx.invalidations - base == 3);
  CHECK(render_lit_pixels(*m, r) > 20);
}

TEST_CASE("gpu usage mode shows a dash and stays quiet without engine statistics") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  bool stats = true;
  auto m = usage_module([&] {
    return ld::GpuUsageSample{stats ? std::optional<double>{50.0} : std::nullopt, std::nullopt};
  });
  m->start(ctx, usage_settings(false));
  loop.advance(3000ms);
  const int with_data = render_lit_pixels(*m, r);
  stats = false;
  const int before = ctx.invalidations;
  loop.advance(1000ms);
  CHECK(ctx.invalidations == before + 1);  // the dash, once
  loop.advance(5000ms);
  CHECK(ctx.invalidations == before + 1);  // then nothing
  CHECK(render_lit_pixels(*m, r) < with_data);  // no graph
  CHECK(render_lit_pixels(*m, r) > 5);          // label + dash
  stats = true;
  loop.advance(1000ms);
  CHECK(ctx.invalidations == before + 2);  // measuring resumes
}

TEST_CASE("without engine statistics, a temperature change still redraws the corner") {
  FakeLoop loop;
  FakeContext ctx(loop);
  int t = 60;
  auto m = usage_module([&] { return ld::GpuUsageSample{std::nullopt, t}; });
  m->start(ctx, usage_settings(true));
  const int base = ctx.invalidations;
  loop.advance(3000ms);
  CHECK(ctx.invalidations == base);
  t = 61;
  loop.advance(1000ms);
  CHECK(ctx.invalidations == base + 1);
}

TEST_CASE("gpu usage mode with show_temperature draws the temperature at the top right") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto plain = usage_module([] { return ld::GpuUsageSample{40.0, 66}; });
  auto hot = usage_module([] { return ld::GpuUsageSample{40.0, 66}; });
  auto cold = usage_module([] { return ld::GpuUsageSample{40.0, std::nullopt}; });
  plain->start(ctx, usage_settings(false));
  hot->start(ctx, usage_settings(true));
  cold->start(ctx, usage_settings(true));
  auto corner = [](ld::Surface& s) { return ink(s, 76, 0, 112, 18).count; };
  auto p = draw(*plain, r);
  auto h = draw(*hot, r);
  auto c = draw(*cold, r);
  CHECK(corner(*p) < corner(*c));  // a dash
  CHECK(corner(*c) < corner(*h));  // "66 °C"
}

TEST_CASE("gpu offers the usage mode, thresholds per measure, and declares its five elements") {
  const auto info = ld::gpu_module_info();
  nlohmann::json out;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json{{"mode", "usage"}}, out));
  CHECK(out["show_temperature"] == false);
  CHECK_FALSE(out.contains("color"));
  CHECK(out["temp_warn"] == 75.0);
  CHECK(out["temp_crit"] == 85.0);
  CHECK(out["vram_warn"] == 90.0);
  CHECK(out["vram_crit"] == 97.0);
  CHECK(out["hysteresis"] == 3.0);
  REQUIRE(info.elements.size() == 5);
  CHECK(std::string(info.elements[3].key) == "temp");
  CHECK(std::string(info.elements[4].key) == "graph");
}

TEST_CASE("gpu temp mode: warning from 75 °C, critical from 85 °C") {
  FakeLoop loop;
  FakeContext ctx(loop);
  int t = 74;
  auto m = ld::make_gpu_module([&] { return std::optional<ld::GpuReading>{ld::GpuReading{4.0, t, 24.0}}; });
  m->start(ctx, settings("temp"));
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Normal);
  t = 75;
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Warning);
  t = 86;
  loop.advance(1000ms);
  const auto v = fixtures::element_of(*m, "value");
  CHECK(v->state == ld::ElementState::Critical);
  CHECK(*v->metric == 86.0);
}

TEST_CASE("gpu vram mode: warning from 90 % of the dedicated memory, nothing without a total") {
  FakeLoop loop;
  FakeContext ctx(loop);
  double used = 21.0, total = 24.0;  // 87.5 %
  auto m = ld::make_gpu_module([&] { return std::optional<ld::GpuReading>{ld::GpuReading{used, 50, total}}; });
  m->start(ctx, settings("vram"));
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Normal);
  used = 22.0;  // 91.7 %
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Warning);
  used = 23.5;  // 97.9 %
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Critical);
  total = 0;  // unknown total: no threshold
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Normal);
}

TEST_CASE("gpu usage mode: the temperature carries its state; the value is stale without statistics") {
  FakeLoop loop;
  FakeContext ctx(loop);
  std::optional<double> pct = 40.0;
  auto m = usage_module([&] { return ld::GpuUsageSample{pct, 80}; });
  m->start(ctx, usage_settings(true));
  CHECK(fixtures::element_of(*m, "temp")->state == ld::ElementState::Warning);
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Normal);
  pct.reset();
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Stale);
  CHECK_FALSE(fixtures::element_of(*m, "graph"));
}

TEST_CASE("gpu vram mode: one decimal, the total under it, the VRAM pink; a temp-mode dash is stale") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{ld::GpuReading{7.81, 50, 24.0}}; });
  m->start(ctx, settings("vram"));
  const auto v = fixtures::element_of(*m, "value");
  CHECK(v->text == L"7.8");
  CHECK(v->color == ld::Rgb{0xff, 0x7a, 0xc8});
  CHECK(fixtures::element_of(*m, "unit")->text == L"/ 24 GB");
  auto none = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; });
  none->start(ctx, settings("temp"));
  CHECK(fixtures::element_of(*none, "value")->state == ld::ElementState::Stale);
}

TEST_CASE("gpu usage mode: a temperature gap resets the state, like cpu_temp") {
  FakeLoop loop;
  FakeContext ctx(loop);
  std::optional<int> temp = 88;
  auto m = usage_module([&] { return ld::GpuUsageSample{40.0, temp}; });
  m->start(ctx, usage_settings(true));
  CHECK(fixtures::element_of(*m, "temp")->state == ld::ElementState::Critical);
  temp.reset();
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "temp")->state == ld::ElementState::Stale);
  temp = 84;  // above crit - hysteresis: would stay Critical without the reset
  loop.advance(1000ms);
  CHECK(fixtures::element_of(*m, "temp")->state == ld::ElementState::Warning);
}

TEST_CASE("gpu vram mode needs no temperature; temp mode shows a dash without one") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto vram = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{ld::GpuReading{5.126, std::nullopt, 8.0}}; });
  vram->start(ctx, settings("vram"));
  CHECK(fixtures::element_of(*vram, "value")->text == L"5.1");
  CHECK(fixtures::element_of(*vram, "value")->state == ld::ElementState::Normal);
  auto temp = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{ld::GpuReading{5.126, std::nullopt, 8.0}}; });
  temp->start(ctx, settings("temp"));
  CHECK(fixtures::element_of(*temp, "value")->text == ld::kDash);
  CHECK(fixtures::element_of(*temp, "value")->state == ld::ElementState::Stale);
  CHECK_FALSE(fixtures::element_of(*temp, "value")->metric);
}

TEST_CASE("gpu vram mode: under 1 GiB of dedicated memory (an iGPU) no total and no thresholds") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = ld::make_gpu_module([] { return std::optional<ld::GpuReading>{ld::GpuReading{0.9, std::nullopt, 0.5}}; });
  m->start(ctx, settings("vram"));
  CHECK(fixtures::element_of(*m, "value")->state == ld::ElementState::Normal);  // 180 % of 0.5 GiB
  CHECK(fixtures::element_of(*m, "unit")->text == L"GB");                       // never "/ 0 GB"
}
