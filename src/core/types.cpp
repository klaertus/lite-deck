#include "core/types.h"
#include <format>

namespace ld {

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::optional<Rgb> parse_color(std::string_view hex) {
  if (hex.size() != 7 || hex[0] != '#') return std::nullopt;
  std::uint8_t v[3];
  for (int i = 0; i < 3; ++i) {
    int hi = hex_digit(hex[1 + i * 2]), lo = hex_digit(hex[2 + i * 2]);
    if (hi < 0 || lo < 0) return std::nullopt;
    v[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return Rgb{v[0], v[1], v[2]};
}

std::string format_color(Rgb c) { return std::format("#{:02x}{:02x}{:02x}", c.r, c.g, c.b); }

}  // namespace ld
