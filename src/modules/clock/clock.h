#pragma once
#include <chrono>
#include <ctime>
#include <string>
#include "modules/module.h"

namespace ld {
std::wstring format_clock(const std::tm& t, bool seconds);
Duration until_next_second(std::chrono::system_clock::time_point now);
Duration until_next_minute(std::chrono::system_clock::time_point now);
ModuleInfo clock_module_info();
}  // namespace ld
