// src/modules/common/wall_clock.cpp
#include "modules/common/wall_clock.h"

namespace ld {
namespace {
std::function<std::chrono::system_clock::time_point()>& frozen() {
  static std::function<std::chrono::system_clock::time_point()> f;
  return f;
}
}  // namespace

std::chrono::system_clock::time_point wall_now() { return frozen() ? frozen()() : std::chrono::system_clock::now(); }

void set_wall_clock(std::function<std::chrono::system_clock::time_point()> clock) { frozen() = std::move(clock); }

}  // namespace ld
