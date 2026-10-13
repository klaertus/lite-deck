#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include "modules/common/state_settings.h"
#include "modules/module.h"

namespace ld {
// Returns the fresh CPU temperature in °C, or nullopt (service absent, error status, stale > 5 s).
// Shared by the cpu_temp key and the cpu_load header, so both change colour and state together.
inline constexpr Rgb kCpuHue{0x00, 0xe0, 0xa0};
inline constexpr Thresholds kCpuTempThresholds{80, 90, 3};
using CpuTempReader = std::function<std::optional<double>()>;
// Reads Global\LiteDeckSensors (opened lazily, reopened while missing); logs state changes only,
// as "<log_tag>: <state>". Each key owns its reader, so each one logs its own changes once.
CpuTempReader service_temperature_reader(std::string_view log_tag = "cpu_temp");
std::unique_ptr<Module> make_cpu_temp_module(CpuTempReader reader);  // empty reader = the service
ModuleInfo cpu_temp_module_info();
}  // namespace ld
