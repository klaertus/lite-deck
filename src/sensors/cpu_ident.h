// src/sensors/cpu_ident.h: what CPUID says about the processor, used to pick a temperature source.
#pragma once
#include <cstdint>
#include <string>

namespace ld::sensors {

struct CpuIdent {
  std::string vendor;  // "AuthenticAMD", "GenuineIntel"...
  std::string brand;   // "AMD Ryzen 7 7700X 8-Core Processor"
  unsigned family = 0, model = 0;  // display family / model (extended fields included)
  // CPUID leaf 6 EAX, the thermal and power features (0 when leaf 6 is not reported). Bit 0: digital
  // temperature sensor, bit 6: package thermal management (Intel).
  std::uint32_t thermal_features = 0;
};
CpuIdent read_cpu_ident();  // defined in pawnio_source.cpp

}  // namespace ld::sensors
