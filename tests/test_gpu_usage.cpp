#include <doctest/doctest.h>
#include <chrono>
#include <thread>
#include "modules/gpu/gpu_stats.h"
#include "modules/gpu/gpu_usage.h"

using ld::GpuNodeSample;

namespace {
constexpr std::int64_t kSecond = 10'000'000;  // 100 ns units
}  // namespace

TEST_CASE("gpu usage: the first sample has no baseline") {
  ld::GpuUsageMeter m;
  CHECK_FALSE(m.update(GpuNodeSample{0, {0, 0}}));
}

TEST_CASE("gpu usage: the busiest engine over the wall time, like Task Manager") {
  ld::GpuUsageMeter m;
  m.update(GpuNodeSample{0, {0, 0, 0}});
  // 3D ran 250 ms, video decode 600 ms, copy idle, in one second.
  auto u = m.update(GpuNodeSample{kSecond, {kSecond / 4, kSecond * 6 / 10, 0}});
  REQUIRE(u);
  CHECK(*u == doctest::Approx(60.0));
  // The next second is measured from the previous sample, not from the start.
  u = m.update(GpuNodeSample{2 * kSecond, {kSecond / 4 + kSecond / 10, kSecond * 6 / 10, 0}});
  REQUIRE(u);
  CHECK(*u == doctest::Approx(10.0));
}

TEST_CASE("gpu usage: an idle GPU is 0 and a saturated one never exceeds 100") {
  ld::GpuUsageMeter m;
  m.update(GpuNodeSample{0, {5, 7}});
  CHECK(*m.update(GpuNodeSample{kSecond, {5, 7}}) == 0.0);
  CHECK(*m.update(GpuNodeSample{2 * kSecond, {5 + kSecond + kSecond / 50, 7}}) == 100.0);  // 102 %: rounding
}

TEST_CASE("gpu usage: samples that cannot be compared give nullopt, then measuring resumes") {
  ld::GpuUsageMeter m;
  m.update(GpuNodeSample{0, {100, 100}});
  CHECK_FALSE(m.update(GpuNodeSample{kSecond, {50, 100}}));     // a counter went back (driver reset)
  CHECK(m.update(GpuNodeSample{2 * kSecond, {50 + kSecond / 2, 100}}) == doctest::Approx(50.0));
  CHECK_FALSE(m.update(GpuNodeSample{3 * kSecond, {0, 0, 0}}));  // engine count changed
  CHECK_FALSE(m.update(GpuNodeSample{3 * kSecond, {0, 0, 0}}));  // no wall time elapsed
  CHECK_FALSE(m.update(GpuNodeSample{2 * kSecond, {0, 0, 0}}));  // the clock went back
  CHECK_FALSE(m.update(GpuNodeSample{4 * kSecond, {}}));         // no engine at all
  CHECK_FALSE(m.update(GpuNodeSample{5 * kSecond, {}}));
}

TEST_CASE("gpu_node_sample never throws (machine dependent result)") {
  CHECK_NOTHROW(ld::gpu_node_sample(0));
  CHECK_FALSE(ld::gpu_node_sample(7).has_value());  // index past the last adapter
  CHECK_NOTHROW(ld::gpu_temperature(0));
  CHECK_FALSE(ld::gpu_temperature(7).has_value());
  if (auto s = ld::gpu_node_sample(0)) {
    CHECK_FALSE(s->running_100ns.empty());
    CHECK(s->wall_100ns > 0);
  }
}

TEST_CASE("manual: the main GPU's usage over one second" * doctest::skip()) {
  ld::GpuUsageMeter m;
  auto a = ld::gpu_node_sample(0);
  REQUIRE(a);
  MESSAGE(a->running_100ns.size() << " engines");
  m.update(std::move(*a));
  std::this_thread::sleep_for(std::chrono::seconds(1));
  auto b = ld::gpu_node_sample(0);
  REQUIRE(b);
  auto u = m.update(std::move(*b));
  REQUIRE(u);
  MESSAGE("GPU usage = " << *u << " % (compare with Task Manager, Performance > GPU)");
  CHECK(*u >= 0.0);
  CHECK(*u <= 100.0);
}

TEST_CASE("gpu adapters are enumerated again after 5 failures in a row, at most once a minute") {
  using std::chrono::seconds;
  ld::GpuRefreshGate g;
  const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  for (int i = 0; i < 4; ++i) CHECK_FALSE(g.failed(t0));
  g.succeeded();  // a success breaks the run
  for (int i = 0; i < 4; ++i) CHECK_FALSE(g.failed(t0));
  CHECK(g.failed(t0));  // the fifth in a row
  for (int i = 0; i < 10; ++i) CHECK_FALSE(g.failed(t0 + seconds(30)));  // rate-limited
  CHECK(g.failed(t0 + seconds(60)));
  for (int i = 0; i < 4; ++i) CHECK_FALSE(g.failed(t0 + seconds(200)));
  CHECK(g.failed(t0 + seconds(200)));
}
