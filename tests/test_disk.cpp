// tests/test_disk.cpp
#include <doctest/doctest.h>
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/format.h"
#include "modules/disk/disk.h"

using namespace std::chrono_literals;

namespace {
constexpr std::uint64_t kGib = 1ull << 30;
struct Drive {
  std::optional<ld::DiskSpace> space = ld::DiskSpace{931 * kGib, 808 * kGib};
  std::optional<ld::DiskIo> io = ld::DiskIo{12'400'000, 3'200'000};
  int space_reads = 0, io_reads = 0;
  std::wstring root;
  bool hold = false;
  std::vector<std::function<void()>> held;
  ld::DiskSpaceReader space_reader() {
    return [this](const std::wstring& r) {
      ++space_reads;
      root = r;
      return space;
    };
  }
  ld::DiskIoReader io_reader() {
    return [this] {
      ++io_reads;
      return io;
    };
  }
  ld::Runner runner() {
    return [this](std::function<void()> job) {
      if (hold) held.push_back(std::move(job));
      else job();
    };
  }
};
std::unique_ptr<fixtures::Rig> start(Drive& d, nlohmann::json over = nlohmann::json::object()) {
  auto rig = std::make_unique<fixtures::Rig>();
  rig->start(ld::disk_module_info(), ld::make_disk_module(d.space_reader(), d.io_reader(), d.runner()), over);
  return rig;
}
std::wstring text(fixtures::Rig& rig, const char* key) {
  auto e = fixtures::element_of(*rig.m, key);
  return e ? e->text : L"<none>";
}
ld::ElementState state(fixtures::Rig& rig, const char* key) { return fixtures::element_of(*rig.m, key)->state; }
void set_free_percent(Drive& d, double percent) {
  d.space = ld::DiskSpace{1000 * kGib, static_cast<std::uint64_t>(percent * 10.0 * static_cast<double>(kGib))};
}
}  // namespace

TEST_CASE("normalize_drive accepts a letter with or without colon and backslash") {
  CHECK(ld::normalize_drive("C:") == L"C:");
  CHECK(ld::normalize_drive("d") == L"D:");
  CHECK(ld::normalize_drive(" e:\\ ") == L"E:");
  CHECK_FALSE(ld::normalize_drive(""));
  CHECK_FALSE(ld::normalize_drive("CD"));
  CHECK_FALSE(ld::normalize_drive("1:"));
  CHECK_FALSE(ld::normalize_drive("\\\\server\\share"));
}

TEST_CASE("disk: schema defaults and validation") {
  const auto info = ld::disk_module_info();
  CHECK(ld::element_spec_errors(info.elements).empty());
  nlohmann::json s;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  CHECK(s["mode"] == "space");
  CHECK(s["drive"] == "C:");
  CHECK(s["free_warn"] == 20);
  CHECK(s["free_crit"] == 10);
  CHECK(ld::apply_schema(info.settings_schema, {{"mode", "smart"}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"free_warn", 101}}, s).has_value());
}

TEST_CASE("disk space: the drive, the percentage used and used / total GB, every 30 s off the loop") {
  Drive d;
  auto rig = start(d);
  CHECK(text(*rig, "label") == L"C:");
  CHECK(text(*rig, "value") == L"13%");
  CHECK(text(*rig, "unit") == L"123 / 931 GB");
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
  CHECK(d.root == L"C:\\");
  CHECK(d.space_reads == 1);
  rig->loop.advance(29s);
  CHECK(d.space_reads == 1);
  rig->loop.advance(1s);
  CHECK(d.space_reads == 2);
  CHECK(d.io_reads == 0);
}

TEST_CASE("disk space: warning below 20 % free, critical below 10 %, with hysteresis") {
  Drive d;
  set_free_percent(d, 30);
  auto rig = start(d, {{"hysteresis", 2}});
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
  set_free_percent(d, 15);
  rig->loop.advance(30s);
  CHECK(state(*rig, "value") == ld::ElementState::Warning);
  CHECK(text(*rig, "value") == L"85%");
  set_free_percent(d, 9);
  rig->loop.advance(30s);
  CHECK(state(*rig, "value") == ld::ElementState::Critical);
  set_free_percent(d, 11);  // above 10 %, not above 10 + 2
  rig->loop.advance(30s);
  CHECK(state(*rig, "value") == ld::ElementState::Critical);
  set_free_percent(d, 21);
  rig->loop.advance(30s);
  CHECK(state(*rig, "value") == ld::ElementState::Warning);
  set_free_percent(d, 23);
  rig->loop.advance(30s);
  CHECK(state(*rig, "value") == ld::ElementState::Normal);
}

TEST_CASE("disk: a missing drive shows a stale dash and comes back") {
  Drive d;
  d.space = std::nullopt;
  auto rig = start(d, {{"drive", "z"}});
  CHECK(text(*rig, "label") == L"Z:");
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  CHECK(text(*rig, "unit") == L"<none>");
  d.space = ld::DiskSpace{100 * kGib, 50 * kGib};
  rig->loop.advance(30s);
  CHECK(text(*rig, "value") == L"50%");
}

TEST_CASE("disk: a stale dash while waiting for the first reading") {
  Drive d;
  d.hold = true;
  auto rig = start(d);
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  rig->loop.advance(60s);
  CHECK(d.held.size() == 1);  // one reading in flight at a time
}

TEST_CASE("disk: a drive setting that is not a letter asks for one") {
  Drive d;
  auto rig = start(d, {{"drive", "C:\\Users"}});
  CHECK(text(*rig, "value") == L"Drive?");
  CHECK(state(*rig, "value") == ld::ElementState::Idle);
  CHECK(d.space_reads == 0);
}

TEST_CASE("disk io: read in the middle, write at the top right, a graph of both") {
  Drive d;
  auto rig = start(d, {{"mode", "io"}, {"drive", "D:"}});
  CHECK(text(*rig, "label") == L"D:");
  CHECK(text(*rig, "value") == L"12.4");
  CHECK(text(*rig, "write") == L"W 3.2 MB/s");
  CHECK(text(*rig, "rate_unit") == L"R MB/s");
  CHECK(d.space_reads == 0);
  d.io = ld::DiskIo{0, 0};
  rig->loop.advance(1s);
  CHECK(text(*rig, "value") == L"0.0");
  const auto graph = fixtures::element_of(*rig->m, "graph");
  REQUIRE(graph);
  CHECK(graph->capacity == 60);
  REQUIRE(graph->points.size() == 2);
  CHECK(graph->points[0] == doctest::Approx(1.0));  // 15.6 MB/s, the largest
  CHECK(graph->points[1] == doctest::Approx(0.0));
  CHECK(d.io_reads == 2);
}

TEST_CASE("disk io: no counters (missing drive) shows a stale dash") {
  Drive d;
  d.io = std::nullopt;
  auto rig = start(d, {{"mode", "io"}});
  CHECK(text(*rig, "value") == ld::kDash);
  CHECK(state(*rig, "value") == ld::ElementState::Stale);
  CHECK(text(*rig, "write") == L"<none>");
}

TEST_CASE("manual: the real disk readers see C: and not a missing drive" * doctest::skip()) {
  CHECK(ld::read_disk_space(L"C:\\"));
  CHECK_FALSE(ld::read_disk_space(L"B:\\"));
  ld::DiskIoReader io = ld::pdh_disk_io_reader(L"C:");
  CHECK_FALSE(io());  // the first sample has no rate
  Sleep(1100);
  const auto r = io();
  REQUIRE(r);
  MESSAGE("C: read " << r->read << " B/s, write " << r->write << " B/s");
}

TEST_CASE("format_disk_rate") {
  CHECK(ld::format_disk_rate(3'240'000) == L"3.2");
  CHECK(ld::format_disk_rate(123'400'000) == L"123");
  CHECK(ld::format_disk_rate(-1) == L"0.0");
}

TEST_CASE("golden: disk space and io") {
  ld::Renderer r;
  {
    Drive d;
    auto rig = start(d);
    auto s = draw(*rig->m, r);
    check_golden(r, *s, "disk_space");
  }
  {
    Drive d;
    auto rig = start(d, {{"mode", "io"}});
    // A quiet minute (under 2 MB/s), a burst of reads 20 s ago, and a copy starting now.
    const double burst[] = {9e6, 26e6, 31e6, 24e6, 8e6};
    const double copy[] = {6e6, 13e6, 17e6, 16e6};
    for (int i = 0; i < 59; ++i) {
      double read = 600'000.0 + 400'000.0 * ((i * 5) % 3);
      if (i >= 36 && i < 41) read = burst[i - 36];
      if (i >= 55) read = copy[i - 55];
      d.io = ld::DiskIo{read, 300'000.0};
      rig->loop.advance(1s);
    }
    d.io = ld::DiskIo{12'400'000, 3'200'000};
    rig->loop.advance(1s);
    auto s = draw(*rig->m, r);
    check_golden(r, *s, "disk_io");
  }
}
