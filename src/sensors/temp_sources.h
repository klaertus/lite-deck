// src/sensors/temp_sources.h: the CPU temperature sources compiled into the sensors service (same
// principle as device/registry.h). To add one: docs/adding-a-temperature-source.md.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "sensors/cpu_ident.h"
#include "sensors/publisher.h"

namespace ld::sensors {

inline constexpr std::size_t kMaxTempSourceIdLength = 64;
inline constexpr std::size_t kMaxModuleNameLength = 64;

struct TempSourceEntry {
  std::string id;      // stable, written in the log: "amd-family17" (a-z, 0-9 and '-')
  std::string name;    // written in the log: "AMD family 17h-1Ah (Tctl/Tdie)"
  std::string module;  // the PawnIO module it loads, a file next to lite-deck-sensors.exe: "AMDFamily17.bin"
  // Whether this source reads this CPU. The first entry that matches is used.
  std::function<bool(const CpuIdent&)> matches;
  // Opens the source with the full path of `module`. Called again every kRetryPeriodMs while it fails.
  std::function<OpenResult(const std::filesystem::path& module_file, const CpuIdent& cpu)> open;
};

// Why `entry` cannot be used (empty = usable): id, name, module file name, matches and open present.
std::vector<std::string> temp_source_errors(const TempSourceEntry& entry);

struct SoundTempSources {
  std::vector<TempSourceEntry> entries;  // the usable ones, in order
  std::vector<std::string> dropped;      // one line per problem, for the log
};
// `entries` without the invalid ones and the repeated ids.
SoundTempSources sound_temp_sources(std::vector<TempSourceEntry> entries);

// The sources compiled in, in priority order. Registering one = one line here (temp_sources.cpp).
std::vector<TempSourceEntry> builtin_temp_sources();

// The first entry whose matches(cpu) is true (one that throws counts as no match), or nullptr.
const TempSourceEntry* choose_temp_source(const CpuIdent& cpu, const std::vector<TempSourceEntry>& entries);

// The result published when no source matches: Status::UnsupportedCpu, "<vendor> family <family>h".
OpenResult unsupported_cpu(const CpuIdent& cpu);

// The service's opener: `chosen` opened with module_dir / chosen->module, or unsupported_cpu(cpu)
// when `chosen` is null. `chosen` must outlive the opener.
SourceOpener temp_source_opener(const std::filesystem::path& module_dir, const CpuIdent& cpu,
                                const TempSourceEntry* chosen);

}  // namespace ld::sensors
