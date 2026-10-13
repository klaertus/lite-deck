#include <doctest/doctest.h>
#include "fake_context.h"
#include "module_fixtures.h"
#include "modules/ram/ram.h"
#include "modules/schema.h"

using namespace std::chrono_literals;

TEST_CASE("used_gib converts bytes to GiB") {
  const std::uint64_t gib = 1ull << 30;
  CHECK(ld::used_gib(32 * gib, 20 * gib) == doctest::Approx(12.0));
  CHECK(ld::used_gib(10, 20) == doctest::Approx(0.0));  // never negative
}

namespace {
constexpr std::uint64_t kGib = 1ull << 30;
struct Setup {
  FakeLoop loop;
  FakeContext ctx{loop};
  std::unique_ptr<ld::Module> m;
  void start(ld::MemoryReader r) {
    auto info = ld::ram_module_info();
    nlohmann::json s;
    REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
    m = ld::make_ram_module(std::move(r));
    m->start(ctx, s);
  }
};
}  // namespace

TEST_CASE("ram module renders a value with the real reader") {
  Setup t;
  ld::Renderer renderer;
  t.start({});
  CHECK(render_lit_pixels(*t.m, renderer) > 20);
}

TEST_CASE("ram module invalidates only when the value changes") {
  Setup t;
  std::uint64_t avail = 20 * kGib;
  t.start([&] { return std::optional(std::make_pair(32 * kGib, avail)); });
  CHECK(t.ctx.invalidations == 1);
  t.loop.advance(5000ms);
  CHECK(t.ctx.invalidations == 1);
  avail = 19 * kGib;
  t.loop.advance(1000ms);
  CHECK(t.ctx.invalidations == 2);
}

TEST_CASE("ram module stays on the dash when the reader fails") {
  Setup t;
  t.start([] { return std::optional<std::pair<std::uint64_t, std::uint64_t>>(); });
  t.loop.advance(3000ms);
  CHECK(t.ctx.invalidations == 0);
}

TEST_CASE("ram: the value warns from 85 % of the memory, is critical from 95 %, and redraws on a state change") {
  Setup t;
  std::uint64_t avail = 8 * kGib;  // 24 of 32 GiB: 75 %
  t.start([&] { return std::optional(std::make_pair(32 * kGib, avail)); });
  CHECK(fixtures::element_of(*t.m, "value")->state == ld::ElementState::Normal);
  avail = 4 * kGib;  // 87.5 %
  t.loop.advance(1000ms);
  CHECK(fixtures::element_of(*t.m, "value")->state == ld::ElementState::Warning);
  avail = 1 * kGib;  // 96.9 %
  t.loop.advance(1000ms);
  CHECK(fixtures::element_of(*t.m, "value")->state == ld::ElementState::Critical);
  const int before = t.ctx.invalidations;
  avail = 1 * kGib + 1;  // same text, same state: nothing to redraw
  t.loop.advance(1000ms);
  CHECK(t.ctx.invalidations == before);
}

TEST_CASE("ram has thresholds and no colour setting") {
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(ld::ram_module_info().settings_schema, nlohmann::json::object(), s));
  CHECK_FALSE(s.contains("color"));
  CHECK(s["warn"] == 85.0);
  CHECK(s["crit"] == 95.0);
}

TEST_CASE("ram shows one decimal and the total under it; a failed reading is stale") {
  Setup t;
  t.start([] { return std::optional(std::make_pair(32 * kGib, 20 * kGib)); });
  CHECK(fixtures::element_of(*t.m, "value")->text == L"12.0");
  CHECK(fixtures::element_of(*t.m, "unit")->text == L"/ 32 GB");
  Setup none;
  none.start([] { return std::optional<std::pair<std::uint64_t, std::uint64_t>>(); });
  CHECK(fixtures::element_of(*none.m, "value")->state == ld::ElementState::Stale);
  CHECK(fixtures::element_of(*none.m, "unit")->text == L"GB");
}
