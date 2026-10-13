#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "sensors/pawnio_source.h"
#include "sensors/tctl.h"

using namespace ld::sensors;

TEST_CASE("read_cpu_ident reads a vendor, a brand and a family") {
  auto id = read_cpu_ident();
  CHECK((id.vendor == "AuthenticAMD" || id.vendor == "GenuineIntel"));
  REQUIRE_FALSE(id.brand.empty());
  CHECK(id.brand.back() != ' ');  // trailing padding trimmed
  CHECK(id.family > 0);
}

TEST_CASE("PawnIoSource::open reports a missing module or an unsupported CPU") {
  auto r = PawnIoSource::open(std::filesystem::path(L"C:\\does-not-exist\\AMDFamily17.bin"));
  CHECK_FALSE(r.source);
  auto id = read_cpu_ident();
  if (amd_family17_supported(id.vendor, id.family))
    CHECK(r.status == Status::ModuleMissing);
  else
    CHECK(r.status == Status::UnsupportedCpu);
}

TEST_CASE("PawnIoSource::open refuses a non-AMD or too-old CPU before touching the module or PawnIO") {
  const auto missing = std::filesystem::path(L"C:\\does-not-exist\\AMDFamily17.bin");
  CpuIdent intel{"GenuineIntel", "Intel(R) Core(TM) i7-12700K", 6, 151};
  auto r = PawnIoSource::open(missing, intel);
  CHECK_FALSE(r.source);
  CHECK(r.status == Status::UnsupportedCpu);  // not ModuleMissing: the CPU check comes first
  CpuIdent old_amd{"AuthenticAMD", "AMD Phenom(tm) II X6 1090T", 0x10, 10};
  CHECK(PawnIoSource::open(missing, old_amd).status == Status::UnsupportedCpu);
  CpuIdent zen4{"AuthenticAMD", "AMD Ryzen 7 7700X 8-Core Processor", 0x19, 0x61};
  CHECK(PawnIoSource::open(missing, zen4).status == Status::ModuleMissing);  // supported: moves on to the module
}

TEST_CASE("pawnio_dll_path ends with PawnIOLib.dll under an absolute folder") {
  auto p = pawnio_dll_path();
  CHECK(p.filename() == L"PawnIOLib.dll");
  CHECK(p.is_absolute());
}

TEST_CASE("program_files_dir is absolute and is the real Program Files") {
  auto pf = program_files_dir();
  REQUIRE(pf.is_absolute());
  wchar_t env[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"ProgramW6432", env, MAX_PATH)) {
    CHECK(_wcsicmp(pf.c_str(), env) == 0);
  }
  auto dll = pawnio_dll_path();
  REQUIRE(dll.is_absolute());
  CHECK(dll.filename() == L"PawnIOLib.dll");
}

TEST_CASE("PawnIoSource::open refuses an oversized module file") {
  const auto tmp = std::filesystem::temp_directory_path() / L"ld_big_module.bin";
  {
    std::ofstream f(tmp, std::ios::binary);
    std::string chunk(1 << 20, 'x');
    f << chunk << "y";
  }
  CpuIdent zen4{"AuthenticAMD", "AMD Ryzen 7 7700X 8-Core Processor", 0x19, 0x61};
  CHECK(PawnIoSource::open(tmp, zen4).status == Status::ModuleMissing);
  std::filesystem::remove(tmp);
}

TEST_CASE("manual: PawnIoSource reads a plausible temperature (admin + PawnIO installed)" * doctest::skip()) {
  auto module = std::filesystem::path(L"E:\\Data\\Project\\lite-deck\\third_party\\pawnio-modules\\AMDFamily17.bin");
  auto r = PawnIoSource::open(module);
  MESSAGE("status = " << status_name(r.status) << ", detail = " << r.detail);
  REQUIRE(r.source);
  auto t = r.source->read_celsius();
  REQUIRE(t);
  MESSAGE("CPU temperature = " << *t << " C");
  CHECK(*t > 20.0);
  CHECK(*t < 100.0);
}
