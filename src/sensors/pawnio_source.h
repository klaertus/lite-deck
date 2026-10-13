// src/sensors/pawnio_source.h
#pragma once
#include <filesystem>
#include <string>
#include "sensors/cpu_ident.h"
#include "sensors/publisher.h"
#include "sensors/temp_sources.h"

namespace ld::sensors {

// InstallLocation of HKLM\...\Uninstall\PawnIO (64-bit view), else %ProgramFiles%\PawnIO; + PawnIOLib.dll.
std::filesystem::path pawnio_dll_path();  // empty if no absolute path can be determined

// The ModuleMissing detail for `module_file`: "<file name> missing, empty or over 1 MB next to the exe".
std::string module_missing_detail(const std::filesystem::path& module_file);

// The 64-bit Program Files folder from the known-folder API (empty on failure).
std::filesystem::path program_files_dir();

// The registry entry of PawnIoSource (id "amd-family17", module AMDFamily17.bin): see temp_sources.h.
TempSourceEntry amd_family17_entry();

// AMD family 17h-1Ah Tctl/Tdie through PawnIO and AMDFamily17.bin. Needs SYSTEM or an administrator.
class PawnIoSource final : public TempSource {
public:
  static OpenResult open(const std::filesystem::path& module_file);
  // Same, with an injected CPU identity (tests).
  static OpenResult open(const std::filesystem::path& module_file, const CpuIdent& cpu);
  ~PawnIoSource() override;
  PawnIoSource(const PawnIoSource&) = delete;
  PawnIoSource& operator=(const PawnIoSource&) = delete;
  std::optional<double> read_celsius() override;

private:
  struct Api;
  PawnIoSource() = default;
  void* dll_ = nullptr;
  void* handle_ = nullptr;     // PawnIO handle
  void* pci_mutex_ = nullptr;  // Global\Access_PCI, may be null
  std::unique_ptr<Api> api_;
  double offset_ = 0;
};

}  // namespace ld::sensors
