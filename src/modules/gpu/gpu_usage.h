// src/modules/gpu/gpu_usage.h
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace ld {

// One reading of an adapter's engines (D3DKMT "nodes"): each node's cumulative running time and the
// wall clock, both in 100 ns units.
struct GpuNodeSample {
  std::int64_t wall_100ns = 0;
  std::vector<std::int64_t> running_100ns;  // one entry per node
};

// Task Manager's GPU %: for each engine, running time over wall time since the previous sample; the
// adapter's value is the busiest engine, clamped to 0-100. nullopt for the first sample and whenever
// two samples cannot be compared (engine count changed, a counter went back after a driver reset, no
// wall time elapsed): the next sample is then measured from this one.
class GpuUsageMeter {
public:
  std::optional<double> update(GpuNodeSample sample);

private:
  std::optional<GpuNodeSample> prev_;
};

// When to enumerate the adapters again: a driver update, a TDR recovery or an eGPU change gives the
// adapter a new LUID, and every query on the cached one fails. After kGpuRefreshAfterFailures failed
// queries in a row, at most once per kGpuRefreshInterval.
inline constexpr int kGpuRefreshAfterFailures = 5;
inline constexpr std::chrono::seconds kGpuRefreshInterval{60};
class GpuRefreshGate {
public:
  void succeeded() { failures_ = 0; }
  // Counts one failure; true when the adapters should be enumerated again now.
  bool failed(std::chrono::steady_clock::time_point now);

private:
  int failures_ = 0;
  std::optional<std::chrono::steady_clock::time_point> last_refresh_;
};

}  // namespace ld
