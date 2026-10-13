#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "fake_context.h"
#include "module_fixtures.h"
#include "core/strings.h"
#include "modules/common/wall_clock.h"
#include "modules/schema.h"
#include "modules/stopwatch/stopwatch.h"
#include "modules/stopwatch/stopwatch_log.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using sc = std::chrono::system_clock;

// The user's real file, as written by the StreamDock plugin.
static const char* kPluginFile =
    "2026 - 10 - 05 - 16:40:30 | 04:09:16\n\n2026 - 10 - 07 - 15:18:42 | 06:02:00\n\nTotal : 10:11:16\n";

TEST_CASE("hms helpers") {
  CHECK(ld::format_hms(3723) == "01:02:03");
  CHECK(ld::format_hms(-61) == "-00:01:01");
  CHECK(ld::format_hms(360000) == "100:00:00");
  CHECK(ld::parse_hms(" 04:09:16") == 14956);
  CHECK(ld::parse_hms("-00:00:10") == -10);
  CHECK_FALSE(ld::parse_hms("abc"));
  CHECK_FALSE(ld::parse_hms("99999999999999999999:00:00"));  // rejected, no overflow
  CHECK(ld::parse_hms("999999999:00:00") == 999999999LL * 3600);
}

TEST_CASE("format_entry matches the plugin") {
  std::tm t{};
  t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 7; t.tm_hour = 15; t.tm_min = 18; t.tm_sec = 42;
  CHECK(ld::format_entry(t, 21720) == "2026 - 10 - 07 - 15:18:42 | 06:02:00");
}

TEST_CASE("sum_entries and append_entry are compatible with the plugin file") {
  CHECK(ld::sum_entries(kPluginFile) == 14956 + 21720);
  std::string out = ld::append_entry(kPluginFile, "2026 - 10 - 11 - 10:00:00 | 01:00:00");
  CHECK(out ==
        "2026 - 10 - 05 - 16:40:30 | 04:09:16\n\n2026 - 10 - 07 - 15:18:42 | 06:02:00\n\n"
        "2026 - 10 - 11 - 10:00:00 | 01:00:00\n\nTotal : 11:11:16\n");
  CHECK(ld::append_entry("", "2026 - 10 - 11 - 10:00:00 | 00:00:05") ==
        "2026 - 10 - 11 - 10:00:00 | 00:00:05\n\nTotal : 00:00:05\n");
  CHECK(ld::sum_entries("a | 01:00:00\r\nb | -00:30:00\r\nTotal : 00:30:00\r\n") == 1800);
}

namespace {
struct Clock {
  sc::time_point t = sc::time_point(std::chrono::hours(500000));
  Clock() { ld::set_wall_clock([this] { return t; }); }
  ~Clock() { ld::set_wall_clock({}); }
};

std::unique_ptr<ld::Module> start(FakeContext& ctx, const fs::path& file, bool seconds_below = false) {
  auto info = ld::stopwatch_module_info();
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema,
                                 nlohmann::json{{"file", ld::narrow(file.wstring())}, {"seconds_below", seconds_below}}, s));
  auto m = info.create();
  m->start(ctx, s);
  return m;
}

std::string read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

TEST_CASE("stopwatch tap starts and pauses; long press saves and stops at zero") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.txt", std::ios::binary) << kPluginFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.txt");
  m->on_event({ld::EventKind::Tap});  // start
  CHECK(ctx.state["running"] == true);
  clock.t += 3661s;
  m->on_event({ld::EventKind::LongPress});  // save 01:01:01 and stop at zero
  std::string content = read(dir / "log.txt");
  CHECK(content.find("| 01:01:01") != std::string::npos);
  CHECK(content.ends_with("Total : 11:12:17\n"));
  CHECK(ctx.state["running"] == false);
  CHECK(ctx.state["accumulated_ms"] == 0);
  int inv = ctx.invalidations;
  loop.advance(3000ms);
  CHECK(ctx.invalidations == inv + 1);  // only the Saved overlay expiry, no per-second ticks
  m->on_event({ld::EventKind::Tap});  // start again
  CHECK(ctx.state["running"] == true);
  clock.t += 10s;
  m->on_event({ld::EventKind::Tap});  // pause
  CHECK(ctx.state["running"] == false);
  CHECK(ctx.state["accumulated_ms"] == 10000);
  fs::remove_all(dir);
}

TEST_CASE("stopwatch restores its state and keeps counting across restarts") {
  Clock clock;
  FakeLoop loop;
  FakeContext ctx(loop);
  fs::path file = fs::temp_directory_path() / "ld_sw_restore.txt";
  ctx.state = {{"running", true},
               {"started_ms", std::chrono::duration_cast<std::chrono::milliseconds>(clock.t.time_since_epoch()).count() - 5000},
               {"accumulated_ms", 1000}};
  auto m = start(ctx, file);
  m->on_event({ld::EventKind::Tap});  // pause -> 6 s accumulated
  CHECK(ctx.state["accumulated_ms"] == 6000);
}

TEST_CASE("stopwatch keeps its time when the log cannot be written") {
  Clock clock;
  FakeLoop loop;
  FakeContext ctx(loop);
  fs::path bad = fs::temp_directory_path() / "ld_no_such_dir" / "sub" / "log.txt";
  fs::remove_all(fs::temp_directory_path() / "ld_no_such_dir");
  auto m = start(ctx, bad);
  m->on_event({ld::EventKind::Tap});
  clock.t += 42s;
  CHECK_NOTHROW(m->on_event({ld::EventKind::LongPress}));
  CHECK(ctx.state["running"] == true);
  m->on_event({ld::EventKind::Tap});  // pause
  CHECK(ctx.state["accumulated_ms"] == 42000);  // not reset
}

TEST_CASE("stopwatch long press at zero shows the total and renders it") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_total";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.txt", std::ios::binary) << kPluginFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = start(ctx, dir / "log.txt");
  int before = ctx.invalidations;
  m->on_event({ld::EventKind::LongPress});
  CHECK(ctx.invalidations == before + 1);
  CHECK(render_lit_pixels(*m, r) > 20);
  CHECK(read(dir / "log.txt") == kPluginFile);  // nothing written
  loop.advance(5100ms);
  CHECK(ctx.invalidations == before + 2);  // back to the normal view
  fs::remove_all(dir);
}

TEST_CASE("stopwatch long press at zero shows Error, not a zero total, when the log cannot be read") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_total_locked";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.txt", std::ios::binary) << kPluginFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.txt");
  HANDLE h = CreateFileW((dir / "log.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  m->on_event({ld::EventKind::LongPress});
  CloseHandle(h);
  const auto label = fixtures::element_of(*m, "label");
  REQUIRE(label);
  CHECK(label->text == L"Error");
  fs::remove_all(dir);
}

TEST_CASE("stopwatch_file defaults to Documents") {
  CHECK(ld::stopwatch_file("").filename() == "stopwatch_log.txt");
  CHECK(ld::stopwatch_file("C:\\x\\y.txt") == fs::path("C:\\x\\y.txt"));
}

TEST_CASE("stopwatch never overwrites a log it cannot read") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_locked";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.txt", std::ios::binary) << kPluginFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.txt");
  m->on_event({ld::EventKind::Tap});
  clock.t += 30s;
  HANDLE h = CreateFileW((dir / "log.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  CHECK_NOTHROW(m->on_event({ld::EventKind::LongPress}));
  CloseHandle(h);
  CHECK(read(dir / "log.txt") == kPluginFile);
  CHECK_FALSE(fs::exists(dir / "log.txt.tmp"));
  CHECK(ctx.state["accumulated_ms"] == 0);  // running: not reset
  m->on_event({ld::EventKind::Tap});
  CHECK(ctx.state["accumulated_ms"] == 30000);
  fs::remove_all(dir);
}

TEST_CASE("stopwatch error path handles non-ASCII paths") {
  Clock clock;
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  fs::path bad = fs::temp_directory_path() / L"ld_no_é_日本" / "log.txt";
  auto m = start(ctx, bad);
  m->on_event({ld::EventKind::Tap});
  clock.t += 5s;
  CHECK_NOTHROW(m->on_event({ld::EventKind::LongPress}));
  CHECK_NOTHROW(render_lit_pixels(*m, r));
}

TEST_CASE("stopwatch tolerates a future start and malformed state") {
  Clock clock;
  {
    FakeLoop loop;
    FakeContext ctx(loop);
    ctx.state = {{"running", true},
                 {"started_ms", std::chrono::duration_cast<std::chrono::milliseconds>(clock.t.time_since_epoch()).count() + 60000},
                 {"accumulated_ms", 2000}};
    auto m = start(ctx, fs::temp_directory_path() / "ld_sw_future.txt");
    m->on_event({ld::EventKind::Tap});  // pause
    CHECK(ctx.state["accumulated_ms"] == 2000);
  }
  {
    FakeLoop loop;
    FakeContext ctx(loop);
    ctx.state = {{"running", "yes"}, {"started_ms", "x"}};
    std::unique_ptr<ld::Module> m;
    CHECK_NOTHROW(m = start(ctx, fs::temp_directory_path() / "ld_sw_bad.txt"));
    m->on_event({ld::EventKind::Tap});
    CHECK(ctx.state["running"] == true);
  }
}

TEST_CASE("stopwatch tap clears the total overlay") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_tapov";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.txt", std::ios::binary) << kPluginFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.txt");
  m->on_event({ld::EventKind::LongPress});  // total overlay
  CHECK_NOTHROW(m->on_event({ld::EventKind::Tap}));
  CHECK(ctx.state["running"] == true);
  int before = ctx.invalidations;
  loop.advance(5100ms);  // old overlay timer must not fire
  CHECK(ctx.invalidations >= before);
  fs::remove_all(dir);
}

TEST_CASE("stopwatch_file expands environment variables and anchors relative paths in Documents") {
  wchar_t temp[MAX_PATH];
  REQUIRE(GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH) > 0);
  CHECK(ld::stopwatch_file("%TEMP%\\sw.txt") == fs::path(temp) / L"sw.txt");
  const fs::path docs = ld::stopwatch_file("").parent_path();
  CHECK(ld::stopwatch_file("logs\\sw.txt") == docs / L"logs" / L"sw.txt");
  CHECK(ld::stopwatch_file("C:\\x\\y.txt") == fs::path(L"C:\\x\\y.txt"));
}

TEST_CASE("stopwatch: running is normal with ►, paused with time is a warning with ❚❚, zero is idle with nothing") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_state";
  fs::remove_all(dir);
  fs::create_directories(dir);
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.txt");
  CHECK(fixtures::element_of(*m, "time")->state == ld::ElementState::Idle);
  CHECK_FALSE(fixtures::element_of(*m, "state"));  // nothing to show at zero
  m->on_event({ld::EventKind::Tap});
  CHECK(fixtures::element_of(*m, "time")->state == ld::ElementState::Normal);
  CHECK(fixtures::element_of(*m, "state")->text == L"►");
  clock.t += 5s;
  m->on_event({ld::EventKind::Tap});
  CHECK(fixtures::element_of(*m, "time")->state == ld::ElementState::Warning);
  CHECK(fixtures::element_of(*m, "state")->text == L"❚❚");
  CHECK(fixtures::element_of(*m, "state")->state == ld::ElementState::Warning);
  fs::remove_all(dir);
}

TEST_CASE("stopwatch settings: the log file and the seconds below, off by default") {
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(ld::stopwatch_module_info().settings_schema, nlohmann::json::object(), s));
  CHECK(s == nlohmann::json{{"file", ""}, {"seconds_below", false}});
  REQUIRE_FALSE(ld::apply_schema(ld::stopwatch_module_info().settings_schema, nlohmann::json{{"file", ""}}, s));
  CHECK(s == nlohmann::json{{"file", ""}, {"seconds_below", false}});  // an older config: the one-line time
}

namespace {
// The texts of `m`'s content by key, in content order.
std::vector<std::pair<std::string, std::wstring>> shown(ld::Module& m) {
  std::vector<std::pair<std::string, std::wstring>> out;
  for (const ld::KeyElement& e : fixtures::content_of(m)) out.emplace_back(e.key, e.text);
  return out;
}
using Shown = std::vector<std::pair<std::string, std::wstring>>;
}  // namespace

TEST_CASE("stopwatch seconds below: hours:minutes and the seconds, in the time's states; ► and ❚❚ unchanged") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_below";
  fs::remove_all(dir);
  fs::create_directories(dir);
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.txt", true);
  CHECK(shown(*m) == Shown{{"hours_minutes", L"00:00"}, {"seconds", L"00"}});
  CHECK(fixtures::element_of(*m, "hours_minutes")->state == ld::ElementState::Idle);
  CHECK(fixtures::element_of(*m, "seconds")->state == ld::ElementState::Idle);
  m->on_event({ld::EventKind::Tap});
  clock.t += 3723s;
  CHECK(shown(*m) == Shown{{"state", L"►"}, {"hours_minutes", L"01:02"}, {"seconds", L"03"}});
  CHECK(fixtures::element_of(*m, "hours_minutes")->state == ld::ElementState::Normal);
  CHECK(fixtures::element_of(*m, "seconds")->state == ld::ElementState::Normal);
  m->on_event({ld::EventKind::Tap});  // pause
  CHECK(shown(*m) == Shown{{"state", L"❚❚"}, {"hours_minutes", L"01:02"}, {"seconds", L"03"}});
  CHECK(fixtures::element_of(*m, "hours_minutes")->state == ld::ElementState::Warning);
  CHECK(fixtures::element_of(*m, "seconds")->state == ld::ElementState::Warning);
  clock.t += 360000s;  // never shown: paused
  CHECK(fixtures::element_of(*m, "hours_minutes")->text == L"01:02");
  m->on_event({ld::EventKind::Tap});
  clock.t += 360000s;  // 101:02:03: more than two digits of hours
  CHECK(shown(*m) == Shown{{"state", L"►"}, {"hours_minutes", L"101:02"}, {"seconds", L"03"}});
  fs::remove_all(dir);
}

TEST_CASE("stopwatch seconds below: the total is split too; off, the one-line time as before") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_below_total";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.txt", std::ios::binary) << kPluginFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto below = start(ctx, dir / "log.txt", true);
  below->on_event({ld::EventKind::LongPress});
  CHECK(shown(*below) == Shown{{"label", L"Total"}, {"hours_minutes", L"10:11"}, {"seconds", L"16"}});
  FakeContext ctx2(loop);
  auto line = start(ctx2, dir / "log.txt");
  CHECK(shown(*line) == Shown{{"time", L"00:00:00"}});
  line->on_event({ld::EventKind::LongPress});
  CHECK(shown(*line) == Shown{{"label", L"Total"}, {"time", L"10:11:16"}});
  fs::remove_all(dir);
}
