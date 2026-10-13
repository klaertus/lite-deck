#pragma once
#include <functional>
#include <memory>
#include <optional>
#include "modules/gpu/gpu_stats.h"
#include "modules/module.h"

namespace ld {
using GpuReader = std::function<std::optional<GpuReading>()>;
// One second of the usage mode: the busiest engine's % (nullopt: no engine statistics, or no
// baseline yet) and the adapter temperature (nullopt when unknown or not shown).
struct GpuUsageSample {
  std::optional<double> percent;
  std::optional<int> temp_c;
};
using GpuUsageReader = std::function<GpuUsageSample()>;
// Readers injected by the tests; empty = D3DKMT (gpu_read, or gpu_node_sample + gpu_temperature).
std::unique_ptr<Module> make_gpu_module(GpuReader reader, GpuUsageReader usage = {});
ModuleInfo gpu_module_info();
}  // namespace ld
