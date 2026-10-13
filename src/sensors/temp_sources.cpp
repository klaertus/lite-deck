// src/sensors/temp_sources.cpp
#include "sensors/temp_sources.h"
#include <format>
#include <set>
#include "sensors/pawnio_source.h"

namespace ld::sensors {

namespace {

bool id_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

bool module_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

bool ends_with_bin(const std::string& s) {
  if (s.size() < 5) return false;  // at least one character before ".bin"
  std::string tail = s.substr(s.size() - 4);
  for (char& c : tail)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return tail == ".bin";
}

}  // namespace

std::vector<std::string> temp_source_errors(const TempSourceEntry& e) {
  std::vector<std::string> errors;
  if (e.id.empty()) errors.push_back("empty id");
  else if (e.id.size() > kMaxTempSourceIdLength)
    errors.push_back(std::format("id longer than {} characters", kMaxTempSourceIdLength));
  else {
    for (char c : e.id)
      if (!id_char(c)) {
        errors.push_back("id: only a-z, 0-9 and '-'");
        break;
      }
  }
  if (e.name.empty()) errors.push_back("empty name");
  if (e.module.empty()) errors.push_back("empty module file name");
  else if (e.module.size() > kMaxModuleNameLength)
    errors.push_back(std::format("module file name longer than {} characters", kMaxModuleNameLength));
  else {
    bool plain = e.module.front() != '.';
    for (char c : e.module)
      if (!module_char(c)) plain = false;
    if (!plain) errors.push_back("module: a plain file name (A-Z, a-z, 0-9, '-', '_', '.'), no folder");
    else if (!ends_with_bin(e.module)) errors.push_back("module: a .bin file");
  }
  if (!e.matches) errors.push_back("no matches function");
  if (!e.open) errors.push_back("no open function");
  return errors;
}

SoundTempSources sound_temp_sources(std::vector<TempSourceEntry> entries) {
  SoundTempSources out;
  std::set<std::string> ids;
  for (TempSourceEntry& e : entries) {
    std::vector<std::string> errors = temp_source_errors(e);
    if (errors.empty() && !ids.insert(e.id).second) errors.push_back("id used twice");
    if (errors.empty()) {
      out.entries.push_back(std::move(e));
      continue;
    }
    std::string shown = e.id.empty() ? std::string("(no id)") : e.id;
    for (char& c : shown)  // control characters would forge log lines
      if (static_cast<unsigned char>(c) < 0x20) c = '?';
    for (const std::string& err : errors)
      out.dropped.push_back(std::format("temperature source {}: {}; the source is not used", shown, err));
  }
  return out;
}

std::vector<TempSourceEntry> builtin_temp_sources() {
  return {
      amd_family17_entry(),
  };
}

const TempSourceEntry* choose_temp_source(const CpuIdent& cpu, const std::vector<TempSourceEntry>& entries) {
  for (const TempSourceEntry& e : entries) {
    bool match = false;
    try {
      match = e.matches && e.matches(cpu);
    } catch (...) {
      match = false;
    }
    if (match) return &e;
  }
  return nullptr;
}

OpenResult unsupported_cpu(const CpuIdent& cpu) {
  return {nullptr, Status::UnsupportedCpu, std::format("{} family {:x}h", cpu.vendor, cpu.family)};
}

SourceOpener temp_source_opener(const std::filesystem::path& module_dir, const CpuIdent& cpu,
                                const TempSourceEntry* chosen) {
  if (!chosen) return [cpu] { return unsupported_cpu(cpu); };
  const std::filesystem::path module_file = module_dir / std::filesystem::path(chosen->module);
  return [chosen, module_file, cpu] { return chosen->open(module_file, cpu); };
}

}  // namespace ld::sensors
