// src/modules/gpu/gpu_usage.cpp
#include "modules/gpu/gpu_usage.h"
#include <algorithm>
#include <utility>

namespace ld {

std::optional<double> GpuUsageMeter::update(GpuNodeSample sample) {
  std::optional<GpuNodeSample> prev = std::exchange(prev_, std::move(sample));
  const GpuNodeSample& now = *prev_;
  if (!prev || now.running_100ns.empty() || prev->running_100ns.size() != now.running_100ns.size())
    return std::nullopt;
  const std::int64_t wall = now.wall_100ns - prev->wall_100ns;
  if (wall <= 0) return std::nullopt;
  double busiest = 0;
  for (std::size_t i = 0; i < now.running_100ns.size(); ++i) {
    const std::int64_t ran = now.running_100ns[i] - prev->running_100ns[i];
    if (ran < 0) return std::nullopt;
    busiest = std::max(busiest, 100.0 * static_cast<double>(ran) / static_cast<double>(wall));
  }
  return std::min(busiest, 100.0);
}

bool GpuRefreshGate::failed(std::chrono::steady_clock::time_point now) {
  if (++failures_ < kGpuRefreshAfterFailures) return false;
  if (last_refresh_ && now - *last_refresh_ < kGpuRefreshInterval) return false;
  last_refresh_ = now;
  failures_ = 0;
  return true;
}

}  // namespace ld
