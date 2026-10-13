#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include "modules/module.h"

namespace ld {
double used_gib(std::uint64_t total_bytes, std::uint64_t avail_bytes);
// Returns (total, avail) bytes; empty reader = GlobalMemoryStatusEx.
using MemoryReader = std::function<std::optional<std::pair<std::uint64_t, std::uint64_t>>()>;
std::unique_ptr<Module> make_ram_module(MemoryReader reader);
ModuleInfo ram_module_info();
}  // namespace ld
