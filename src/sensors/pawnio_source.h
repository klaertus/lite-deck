// src/sensors/pawnio_source.h
#pragma once
#include <filesystem>
#include <string>
#include "sensors/publisher.h"

namespace ld::sensors {

struct CpuIdent {
  std::string vendor;  // "AuthenticAMD", "GenuineIntel"...
  std::string brand;   // "AMD Ryzen 7 7700X 8-Core Processor"
  unsigned family = 0, model = 0;
};
CpuIdent read_cpu_ident();

// InstallLocation of HKLM\...\Uninstall\PawnIO (64-bit view), else %ProgramFiles%\PawnIO; + PawnIOLib.dll.
std::filesystem::path pawnio_dll_path();  // empty if no absolute path can be determined

// The 64-bit Program Files folder from the known-folder API (empty on failure).
std::filesystem::path program_files_dir();

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
