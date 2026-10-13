#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include "modules/cpu_temp/cpu_temp.h"
#include "modules/module.h"

namespace ld {
struct CpuTimes {
  std::uint64_t idle = 0, kernel = 0, user = 0;  // 100 ns units; kernel includes idle
};
double cpu_busy_percent(const CpuTimes& before, const CpuTimes& after);
std::optional<CpuTimes> read_cpu_times();  // nullopt when GetSystemTimes fails
// `temp_reader` is used only when show_temperature is set; empty = the sensors service reader.
std::unique_ptr<Module> make_cpu_load_module(CpuTempReader temp_reader);
ModuleInfo cpu_load_module_info();
}  // namespace ld
