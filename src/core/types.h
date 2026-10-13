#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ld {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;
using TimerId = std::uint64_t;

struct Rgb {
  std::uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb&) const = default;
};

std::optional<Rgb> parse_color(std::string_view hex);  // "#rrggbb"
std::string format_color(Rgb c);

}  // namespace ld
