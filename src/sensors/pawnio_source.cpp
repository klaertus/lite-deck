// src/sensors/pawnio_source.cpp
#include "sensors/pawnio_source.h"
#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <intrin.h>
#include <sddl.h>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>
#include "sensors/tctl.h"

namespace ld::sensors {

// Signatures from PawnIOLib.h (namazso/PawnIO): sizes are counts of ULONG64, not bytes.
struct PawnIoSource::Api {
  using Open = HRESULT(__stdcall*)(PHANDLE);
  using Load = HRESULT(__stdcall*)(HANDLE, const UCHAR*, SIZE_T);
  using Execute = HRESULT(__stdcall*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
  using Close = HRESULT(__stdcall*)(HANDLE);
  Open open = nullptr;
  Load load = nullptr;
  Execute execute = nullptr;
  Close close = nullptr;
};

namespace {

std::string hr_text(const char* what, HRESULT hr) {
  return std::format("{} hr=0x{:08X}", what, static_cast<unsigned long>(hr));
}

std::vector<unsigned char> read_file(const std::filesystem::path& p) {
  constexpr std::uintmax_t kMaxModuleBytes = 1u << 20;  // the real module is a few KB
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  if (ec || size == 0 || size > kMaxModuleBytes) return {};
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::vector<unsigned char> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (v.size() > kMaxModuleBytes) return {};
  return v;
}

}  // namespace

CpuIdent read_cpu_ident() {
  CpuIdent id;
  int r[4] = {};
  __cpuid(r, 0);
  char vendor[13] = {};
  std::memcpy(vendor + 0, &r[1], 4);  // EBX
  std::memcpy(vendor + 4, &r[3], 4);  // EDX
  std::memcpy(vendor + 8, &r[2], 4);  // ECX
  id.vendor = vendor;
  __cpuid(r, 1);
  id.family = cpu_family(static_cast<std::uint32_t>(r[0]));
  id.model = cpu_model(static_cast<std::uint32_t>(r[0]));
  __cpuid(r, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(r[0]) >= 0x80000004) {
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
      __cpuid(r, static_cast<int>(0x80000002 + i));
      std::memcpy(brand + 16 * i, r, 16);
    }
    id.brand = brand;
    while (!id.brand.empty() && (id.brand.back() == ' ' || id.brand.back() == '\0')) id.brand.pop_back();
    const auto first = id.brand.find_first_not_of(' ');
    id.brand = first == std::string::npos ? std::string() : id.brand.substr(first);
  }
  return id;
}

std::filesystem::path pawnio_dll_path() {
  wchar_t buf[MAX_PATH] = {};
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
                   L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) == ERROR_SUCCESS &&
      buf[0] != L'\0') {
    std::filesystem::path dir(buf);  // with or without a trailing backslash
    if (dir.is_absolute()) return dir / L"PawnIOLib.dll";
  }
  const auto pf = program_files_dir();
  if (pf.empty()) return {};
  return pf / L"PawnIO" / L"PawnIOLib.dll";
}

std::filesystem::path program_files_dir() {
  // The known-folder API, never an environment variable: the service runs as SYSTEM.
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
    std::filesystem::path p(raw);
    if (p.is_absolute()) result = std::move(p);
  }
  CoTaskMemFree(raw);
  return result;
}

TempSourceEntry amd_family17_entry() {
  return TempSourceEntry{
      .id = "amd-family17",
      .name = "AMD family 17h-1Ah (Tctl/Tdie)",
      .module = "AMDFamily17.bin",
      .matches = [](const CpuIdent& cpu) { return amd_family17_supported(cpu.vendor, cpu.family); },
      .open = [](const std::filesystem::path& module_file, const CpuIdent& cpu) {
        return PawnIoSource::open(module_file, cpu);
      },
  };
}

OpenResult PawnIoSource::open(const std::filesystem::path& module_file) {
  return open(module_file, read_cpu_ident());
}

OpenResult PawnIoSource::open(const std::filesystem::path& module_file, const CpuIdent& cpu) {
  if (!amd_family17_supported(cpu.vendor, cpu.family)) return unsupported_cpu(cpu);

  const std::vector<unsigned char> blob = read_file(module_file);
  if (blob.empty()) return {nullptr, Status::ModuleMissing, "AMDFamily17.bin missing, empty or over 1 MB next to the exe"};

  std::unique_ptr<PawnIoSource> s(new PawnIoSource());
  s->offset_ = tdie_offset(cpu.brand);
  s->api_ = std::make_unique<Api>();
  const auto dll = pawnio_dll_path();
  if (dll.empty() || !dll.is_absolute()) return {nullptr, Status::PawnIoMissing, "no absolute PawnIO install path"};
  HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!m) return {nullptr, Status::PawnIoMissing, std::format("LoadLibrary error {}", GetLastError())};
  s->dll_ = m;
  s->api_->open = reinterpret_cast<Api::Open>(GetProcAddress(m, "pawnio_open"));
  s->api_->load = reinterpret_cast<Api::Load>(GetProcAddress(m, "pawnio_load"));
  s->api_->execute = reinterpret_cast<Api::Execute>(GetProcAddress(m, "pawnio_execute"));
  s->api_->close = reinterpret_cast<Api::Close>(GetProcAddress(m, "pawnio_close"));
  if (!s->api_->open || !s->api_->load || !s->api_->execute || !s->api_->close)
    return {nullptr, Status::PawnIoMissing, "PawnIOLib.dll without the expected exports"};

  HANDLE h = nullptr;
  HRESULT hr = s->api_->open(&h);
  if (FAILED(hr)) {
    const bool denied = hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == E_ACCESSDENIED;
    return {nullptr, denied ? Status::AccessDenied : Status::PawnIoMissing, hr_text("pawnio_open", hr)};
  }
  s->handle_ = h;
  hr = s->api_->load(h, blob.data(), blob.size());
  if (FAILED(hr)) return {nullptr, Status::ModuleRejected, hr_text("pawnio_load", hr)};

  // Same cross-tool PCI lock as LibreHardwareMonitor (AMDFamily17.p asks for it around ioctl_read_smn).
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr)) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    s->pci_mutex_ = CreateMutexW(&sa, FALSE, L"Global\\Access_PCI");
    LocalFree(sd);
  }
  if (!s->pci_mutex_) s->pci_mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");

  return {std::move(s), Status::Ok, {}};
}

PawnIoSource::~PawnIoSource() {
  if (handle_ && api_ && api_->close) api_->close(static_cast<HANDLE>(handle_));
  if (pci_mutex_) CloseHandle(static_cast<HANDLE>(pci_mutex_));
  if (dll_) FreeLibrary(static_cast<HMODULE>(dll_));
}

std::optional<double> PawnIoSource::read_celsius() {
  bool locked = false;
  if (pci_mutex_) {
    const DWORD w = WaitForSingleObject(static_cast<HANDLE>(pci_mutex_), 250);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return std::nullopt;
    locked = true;
  }
  const ULONG64 in = kThmTconCurTmp;
  ULONG64 out = 0;
  SIZE_T n = 0;
  const HRESULT hr = api_->execute(static_cast<HANDLE>(handle_), "ioctl_read_smn", &in, 1, &out, 1, &n);
  if (locked) ReleaseMutex(static_cast<HANDLE>(pci_mutex_));
  if (FAILED(hr) || n != 1) return std::nullopt;
  return tctl_from_register(static_cast<std::uint32_t>(out)) + offset_;
}

}  // namespace ld::sensors
