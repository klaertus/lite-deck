// src/sensors/cpu_ident.h: what CPUID says about the processor, used to pick a temperature source.
#pragma once
#include <string>

namespace ld::sensors {

struct CpuIdent {
  std::string vendor;  // "AuthenticAMD", "GenuineIntel"...
  std::string brand;   // "AMD Ryzen 7 7700X 8-Core Processor"
  unsigned family = 0, model = 0;  // display family / model (extended fields included)
};
CpuIdent read_cpu_ident();  // defined in pawnio_source.cpp

}  // namespace ld::sensors
