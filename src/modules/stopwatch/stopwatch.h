// src/modules/stopwatch/stopwatch.h
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include "modules/module.h"

namespace ld {
std::filesystem::path stopwatch_file(const std::string& setting);  // "" -> Documents\stopwatch_log.txt
ModuleInfo stopwatch_module_info();
}  // namespace ld
