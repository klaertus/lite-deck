// tests/test_temp_sources.cpp: the CPU temperature source registry and how the service picks a source.
#include <doctest/doctest.h>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "sensors/pawnio_source.h"
#include "sensors/publisher.h"
#include "sensors/temp_sources.h"

using namespace ld::sensors;

namespace {

const CpuIdent kZen1{"AuthenticAMD", "AMD Ryzen 7 1700X Eight-Core Processor", 0x17, 0x01};
const CpuIdent kZen4{"AuthenticAMD", "AMD Ryzen 7 7700X 8-Core Processor", 0x19, 0x61};
const CpuIdent kZen5{"AuthenticAMD", "AMD Ryzen 7 9700X 8-Core Processor", 0x1A, 0x44};
const CpuIdent kPhenom{"AuthenticAMD", "AMD Phenom(tm) II X6 1090T", 0x10, 10};
// Reports the digital temperature sensor and package thermal management (CPUID.06H:EAX bits 0 and 6).
const CpuIdent kAlderLake{"GenuineIntel", "12th Gen Intel(R) Core(TM) i7-12700K", 6, 151, (1u << 0) | (1u << 6)};

struct FixedSource final : TempSource {
  double value;
  explicit FixedSource(double v) : value(v) {}
  std::optional<double> read_celsius() override { return value; }
};

// A source a contributor could add: matches Intel, never touches PawnIO.
TempSourceEntry fake_intel_entry(std::filesystem::path* opened_with = nullptr) {
  return TempSourceEntry{
      .id = "fake-intel",
      .name = "Fake Intel package temperature",
      .module = "IntelMSR.bin",
      .matches = [](const CpuIdent& cpu) { return cpu.vendor == "GenuineIntel"; },
      .open = [opened_with](const std::filesystem::path& module_file, const CpuIdent&) {
        if (opened_with) *opened_with = module_file;
        return OpenResult{std::make_unique<FixedSource>(42.5), Status::Ok, {}};
      },
  };
}

TempSourceEntry valid_entry(std::string id) {
  TempSourceEntry e = fake_intel_entry();
  e.id = std::move(id);
  return e;
}

}  // namespace

TEST_CASE("the built-in temperature sources are valid, with unique ids and a module each") {
  const std::vector<TempSourceEntry> builtin = builtin_temp_sources();
  REQUIRE_FALSE(builtin.empty());
  std::set<std::string> ids;
  for (const TempSourceEntry& e : builtin) {
    CAPTURE(e.id);
    CHECK(temp_source_errors(e).empty());
    CHECK(ids.insert(e.id).second);
    CHECK_FALSE(e.module.empty());
  }
  const SoundTempSources sound = sound_temp_sources(builtin_temp_sources());
  CHECK(sound.dropped.empty());
  CHECK(sound.entries.size() == builtin.size());
}

TEST_CASE("the AMD source keeps its id and its module (AMDFamily17.bin)") {
  const TempSourceEntry amd = amd_family17_entry();
  CHECK(amd.id == "amd-family17");
  CHECK(amd.module == "AMDFamily17.bin");
  CHECK_FALSE(amd.name.empty());
}

TEST_CASE("temp_source_errors checks the id, the name, the module file name and the functions") {
  CHECK(temp_source_errors(valid_entry("ok-1")).empty());
  auto broken = [](auto change) {
    TempSourceEntry e = valid_entry("ok");
    change(e);
    return temp_source_errors(e);
  };
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.id.clear(); }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.id = std::string(65, 'a'); }).empty());
  CHECK(broken([](TempSourceEntry& e) { e.id = std::string(64, 'a'); }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.id = "Intel"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.id = "intel msr"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.id = "intel\n"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.name.clear(); }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module.clear(); }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = "IntelMSR.dll"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = ".bin"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = "..\\IntelMSR.bin"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = "sub/IntelMSR.bin"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = "C:IntelMSR.bin"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = "Intel MSR.bin"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.module = std::string(61, 'a') + ".bin"; }).empty());
  CHECK(broken([](TempSourceEntry& e) { e.module = "Intel_MSR-2.BIN"; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.matches = nullptr; }).empty());
  CHECK_FALSE(broken([](TempSourceEntry& e) { e.open = nullptr; }).empty());
}

TEST_CASE("sound_temp_sources drops invalid entries and repeated ids, keeping the order") {
  TempSourceEntry bad = valid_entry("bad");
  bad.module = "x.dll";
  TempSourceEntry no_id = valid_entry("");
  const SoundTempSources s =
      sound_temp_sources({valid_entry("first"), bad, valid_entry("second"), valid_entry("first"), no_id});
  REQUIRE(s.entries.size() == 2);
  CHECK(s.entries[0].id == "first");
  CHECK(s.entries[1].id == "second");
  REQUIRE(s.dropped.size() == 3);
  CHECK(s.dropped[0].find("bad") != std::string::npos);
  CHECK(s.dropped[1].find("id used twice") != std::string::npos);
  CHECK(s.dropped[2].find("(no id)") != std::string::npos);
}

TEST_CASE("choose_temp_source: AMD family 17h, 19h and 1Ah pick the AMD source; Intel and older AMD pick none") {
  const SoundTempSources s = sound_temp_sources(builtin_temp_sources());
  for (const CpuIdent& amd : {kZen1, kZen4, kZen5}) {
    CAPTURE(amd.family);
    const TempSourceEntry* e = choose_temp_source(amd, s.entries);
    REQUIRE(e);
    CHECK(e->id == "amd-family17");
  }
  CHECK(choose_temp_source(kAlderLake, s.entries) == nullptr);
  CHECK(choose_temp_source(kPhenom, s.entries) == nullptr);
  CHECK(choose_temp_source(kZen4, {}) == nullptr);
}

TEST_CASE("choose_temp_source takes the first match and skips a matches() that throws") {
  TempSourceEntry thrower = valid_entry("thrower");
  thrower.matches = [](const CpuIdent&) -> bool { throw std::runtime_error("boom"); };
  std::vector<TempSourceEntry> entries{thrower, valid_entry("one"), valid_entry("two")};
  const TempSourceEntry* e = choose_temp_source(kAlderLake, entries);
  REQUIRE(e);
  CHECK(e->id == "one");
}

TEST_CASE("no matching source: the service publishes unsupported CPU, with today's detail") {
  const OpenResult r = temp_source_opener(L"C:\\does-not-exist", kAlderLake, nullptr)();
  CHECK_FALSE(r.source);
  CHECK(r.status == Status::UnsupportedCpu);
  CHECK(r.detail == "GenuineIntel family 6h");
  // Same result as PawnIoSource itself gives for that CPU.
  const OpenResult direct = PawnIoSource::open(L"C:\\does-not-exist\\AMDFamily17.bin", kAlderLake);
  CHECK(direct.status == r.status);
  CHECK(direct.detail == r.detail);

  SensorBlock block{};
  Publisher pub(block, temp_source_opener(L"C:\\does-not-exist", kAlderLake, nullptr));
  pub.tick(1'000);
  CHECK(pub.status() == Status::UnsupportedCpu);
}

TEST_CASE("the AMD source opens AMDFamily17.bin from the given folder (missing here: ModuleMissing, no PawnIO)") {
  const SoundTempSources s = sound_temp_sources(builtin_temp_sources());
  const TempSourceEntry* amd = choose_temp_source(kZen4, s.entries);
  REQUIRE(amd);
  const OpenResult r = temp_source_opener(L"C:\\does-not-exist", kZen4, amd)();
  CHECK_FALSE(r.source);
  CHECK(r.status == Status::ModuleMissing);  // the module is read before PawnIO is loaded
  CHECK(r.detail == "AMDFamily17.bin missing, empty or over 1 MB next to the exe");  // today's text
}

TEST_CASE("the module-missing detail names the module the source needs") {
  CHECK(module_missing_detail(L"C:\\lite-deck\\IntelMSR.bin") == "IntelMSR.bin missing, empty or over 1 MB next to the exe");
  CHECK(module_missing_detail(L"C:\\lite-deck\\AMDFamily17.bin") ==
        "AMDFamily17.bin missing, empty or over 1 MB next to the exe");
  CHECK(module_missing_detail({}) == "the PawnIO module missing, empty or over 1 MB next to the exe");
}

TEST_CASE("a source registered next to the built-in ones is picked and published without other changes") {
  std::filesystem::path opened_with;
  std::vector<TempSourceEntry> list = builtin_temp_sources();
  list.push_back(fake_intel_entry(&opened_with));  // the one line a contributor adds
  const SoundTempSources s = sound_temp_sources(std::move(list));
  REQUIRE(s.dropped.empty());

  const TempSourceEntry* chosen = choose_temp_source(kAlderLake, s.entries);
  REQUIRE(chosen);
  CHECK(chosen->id == "fake-intel");
  const TempSourceEntry* amd = choose_temp_source(kZen4, s.entries);
  REQUIRE(amd);
  CHECK(amd->id == "amd-family17");  // AMD machines still get the AMD source

  SensorBlock block{};
  Publisher pub(block, temp_source_opener(L"C:\\lite-deck", kAlderLake, chosen));
  pub.tick(5'000);
  CHECK(opened_with == std::filesystem::path(L"C:\\lite-deck\\IntelMSR.bin"));
  CHECK(pub.status() == Status::Ok);
  const auto snap = read_block(block);
  REQUIRE(snap);
  CHECK(snap->status == Status::Ok);
  CHECK(snap->cpu_temp_c == doctest::Approx(42.5));
  CHECK(snap->updated_unix_ms == 5'000);
}
