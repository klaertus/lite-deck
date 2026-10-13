// src/sensors/tctl.cpp
#include "sensors/tctl.h"

namespace ld::sensors {

double tctl_from_register(std::uint32_t raw) {
  const bool offset = (raw & kTempRangeSelMask) != 0 || (raw & kTempTjSelMask) == kTempTjSelMask;
  double t = static_cast<double>(raw >> 21) * 0.125;
  if (offset) t -= 49.0;
  return t;
}

double tdie_offset(std::string_view b) {
  auto has = [b](std::string_view s) { return b.find(s) != std::string_view::npos; };
  if (has("1600X") || has("1700X") || has("1800X")) return -20.0;
  if (has("Threadripper 19") || has("Threadripper 29")) return -27.0;
  if (has("2700X")) return -10.0;
  return 0.0;
}

unsigned cpu_family(std::uint32_t eax) {
  const unsigned base = (eax >> 8) & 0xF;
  return base == 0xF ? base + ((eax >> 20) & 0xFF) : base;
}

unsigned cpu_model(std::uint32_t eax) {
  const unsigned base_family = (eax >> 8) & 0xF;
  unsigned model = (eax >> 4) & 0xF;
  if (base_family == 0xF || base_family == 0x6) model |= ((eax >> 16) & 0xF) << 4;
  return model;
}

bool amd_family17_supported(std::string_view vendor, unsigned family) {
  return vendor == "AuthenticAMD" && family >= 0x17 && family <= 0x1A;
}

}  // namespace ld::sensors
