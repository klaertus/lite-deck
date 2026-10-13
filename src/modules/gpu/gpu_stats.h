#pragma once
#include <optional>
#include "modules/gpu/gpu_usage.h"

namespace ld {
struct GpuReading {
  double vram_used_gib = 0;
  std::optional<int> temp_c;  // °C; nullopt when not asked for or not reported by the driver
  double vram_total_gib = 0;  // DXGI_ADAPTER_DESC1::DedicatedVideoMemory; 0 = unknown
};
// Reads the VRAM in use through D3DKMT (the WDDM kernel interface, no vendor DLL), and the temperature
// too when `with_temperature`. `index` = hardware adapter in decreasing order of dedicated video memory.
// nullopt when the VRAM statistics are unavailable; a missing temperature alone leaves temp_c empty.
std::optional<GpuReading> gpu_read(unsigned index, bool with_temperature = false);
// The engine counters of hardware adapter `index` (same order as gpu_read): D3DKMT_QUERYSTATISTICS_NODE
// GlobalInformation.RunningTime per node, and QueryPerformanceCounter for the wall time. nullopt when the
// driver publishes no node statistics (virtual or remote display adapters, some old drivers).
std::optional<GpuNodeSample> gpu_node_sample(unsigned index);
// The adapter temperature alone (°C), for keys that do not show the VRAM. nullopt when the driver does
// not report it (a reported 0 °C counts as not reported).
std::optional<int> gpu_temperature(unsigned index);
}  // namespace ld
