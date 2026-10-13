// src/modules/common/wall_clock.h
#pragma once
#include <chrono>
#include <functional>

namespace ld {
// The wall clock read by the clock, date and stopwatch modules: system_clock::now() unless a test froze it.
std::chrono::system_clock::time_point wall_now();
void set_wall_clock(std::function<std::chrono::system_clock::time_point()> clock);  // tests; {} = real
}  // namespace ld
