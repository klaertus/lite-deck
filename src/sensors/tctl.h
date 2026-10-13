// src/sensors/tctl.h
#pragma once
#include <cstdint>
#include <string_view>

namespace ld::sensors {

// AMD family 17h+ SMN thermal register (THM_TCON_CUR_TMP), as read by LibreHardwareMonitor.
inline constexpr std::uint32_t kThmTconCurTmp = 0x00059800;
inline constexpr std::uint32_t kTempRangeSelMask = 0x80000;  // bit 19: value is offset by +49 °C
inline constexpr std::uint32_t kTempTjSelMask = 0x30000;     // both bits set: same -49 °C offset

// Raw register -> Tctl in °C: bits 31:21 in 1/8 °C, minus 49 when range select (or both TJ_SEL bits) is set.
double tctl_from_register(std::uint32_t raw);

// Tctl -> Tdie offset (°C, 0 or negative) for the models that report a raised Tctl, matched on the brand string.
double tdie_offset(std::string_view cpu_brand);

// CPUID leaf 1 EAX -> display family / model (extended fields included).
unsigned cpu_family(std::uint32_t cpuid1_eax);
unsigned cpu_model(std::uint32_t cpuid1_eax);

// What AMDFamily17.bin accepts at load: AMD, family 17h to 1Ah.
bool amd_family17_supported(std::string_view vendor, unsigned family);

}  // namespace ld::sensors
