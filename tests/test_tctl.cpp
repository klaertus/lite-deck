#include <doctest/doctest.h>
#include "sensors/tctl.h"

using namespace ld::sensors;

TEST_CASE("tctl_from_register: bits 31:21 are eighths of a degree") {
  CHECK(tctl_from_register(400u << 21) == 50.0);           // 400 / 8
  CHECK(tctl_from_register(401u << 21) == 50.125);
  CHECK(tctl_from_register(0x7FFu << 21) == 255.875);      // maximum raw value
  CHECK(tctl_from_register(0) == 0.0);
  CHECK(tctl_from_register((400u << 21) | 0x1FFFF) == 50.0);  // low bits ignored (TJ_SEL incomplete)
}

TEST_CASE("tctl_from_register: range select subtracts 49 degrees") {
  CHECK(tctl_from_register((792u << 21) | kTempRangeSelMask) == 50.0);  // 99 - 49
}

TEST_CASE("tctl_from_register: both TJ_SEL bits also subtract 49 degrees, one bit does not") {
  CHECK(tctl_from_register((792u << 21) | kTempTjSelMask) == 50.0);
  CHECK(tctl_from_register((400u << 21) | 0x10000) == 50.0);
  CHECK(tctl_from_register((400u << 21) | 0x20000) == 50.0);
}

TEST_CASE("tdie_offset follows LibreHardwareMonitor's table") {
  CHECK(tdie_offset("AMD Ryzen 7 1700X Eight-Core Processor") == -20.0);
  CHECK(tdie_offset("AMD Ryzen 5 1600X Six-Core Processor") == -20.0);
  CHECK(tdie_offset("AMD Ryzen 7 1800X Eight-Core Processor") == -20.0);
  CHECK(tdie_offset("AMD Ryzen Threadripper 1950X 16-Core Processor") == -27.0);
  CHECK(tdie_offset("AMD Ryzen Threadripper 2990WX 32-Core Processor") == -27.0);
  CHECK(tdie_offset("AMD Ryzen 7 2700X Eight-Core Processor") == -10.0);
  CHECK(tdie_offset("AMD Ryzen 7 7700X 8-Core Processor") == 0.0);
  CHECK(tdie_offset("") == 0.0);
}

TEST_CASE("cpu_family and cpu_model decode CPUID leaf 1") {
  CHECK(cpu_family(0x00A60F12) == 0x19);  // Ryzen 7 7700X: family 25, model 97
  CHECK(cpu_model(0x00A60F12) == 0x61);
  CHECK(cpu_family(0x00870F10) == 0x17);  // Zen 2 (Matisse)
  CHECK(cpu_model(0x00870F10) == 0x71);
  CHECK(cpu_family(0x000906EA) == 6);     // Intel Coffee Lake
  CHECK(cpu_model(0x000906EA) == 0x9E);
}

TEST_CASE("amd_family17_supported accepts only AMD 17h-1Ah") {
  CHECK(amd_family17_supported("AuthenticAMD", 0x17));
  CHECK(amd_family17_supported("AuthenticAMD", 0x19));
  CHECK(amd_family17_supported("AuthenticAMD", 0x1A));
  CHECK_FALSE(amd_family17_supported("AuthenticAMD", 0x15));
  CHECK_FALSE(amd_family17_supported("AuthenticAMD", 0x1B));
  CHECK_FALSE(amd_family17_supported("GenuineIntel", 0x19));
  CHECK_FALSE(amd_family17_supported("", 0));
}
