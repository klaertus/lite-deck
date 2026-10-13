// src/modules/sun/sun.h
#pragma once
#include <cstdint>
#include <memory>
#include "modules/module.h"
#include "modules/sun/sun_calc.h"

namespace ld {

// The PC's time zone: localtime_s, and mktime with tm_isdst = -1 for midnights.
LocalZone system_zone();

// Wakes up at least this often to notice a clock change or a resume from sleep.
inline constexpr std::int64_t kSunMaxSleepS = 600;

// `zone` injected by the tests; empty functions = system_zone().
std::unique_ptr<Module> make_sun_module(LocalZone zone);
ModuleInfo sun_module_info();

}  // namespace ld
