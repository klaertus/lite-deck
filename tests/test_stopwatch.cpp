#include <doctest/doctest.h>
#include <windows.h>
#include <cstring>
#include <ctime>
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

// A log as written by the module: BOM, header, CRLF lines. Active total 10:11:16.
static const char* kCsvFile =
    "\xEF\xBB\xBF" "date;start;end;active;paused;pauses\r\n"
    "2026-10-05;12:31:14;16:40:30;4:09:16;0:00:00;0\r\n"
    "2026-10-07;09:00:00;15:18:42;6:02:00;0:16:42;3\r\n";

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

TEST_CASE("csv durations and rows") {
  CHECK(ld::format_duration(0) == "0:00:00");
  CHECK(ld::format_duration(3723) == "1:02:03");
  CHECK(ld::format_duration(26 * 3600 + 190) == "26:03:10");
  CHECK(ld::format_duration(-5) == "0:00:00");
  ld::CsvRow r;
  r.start.tm_year = 126; r.start.tm_mon = 9; r.start.tm_mday = 5; r.start.tm_hour = 9; r.start.tm_min = 3; r.start.tm_sec = 7;
  r.end.tm_hour = 17; r.end.tm_min = 30;
  r.active_s = 7 * 3600 + 45 * 60;
  r.paused_s = 45 * 60;
  r.pauses = 2;
  CHECK(ld::format_csv_row(r) == "2026-10-05;09:03:07;17:30:00;7:45:00;0:45:00;2");
}

TEST_CASE("sum_csv_active sums the active column and skips the header and malformed lines") {
  CHECK(ld::sum_csv_active(kCsvFile) == 14956 + 21720);
  CHECK(ld::sum_csv_active("") == 0);
  CHECK(ld::sum_csv_active("date;start;end;active;paused;pauses\n") == 0);
  CHECK(ld::sum_csv_active("2026-10-05;09:00:00;10:00:00;1:00:00;0:00:00;0") == 3600);  // no final newline
  CHECK(ld::sum_csv_active("a;b;c;26:03:10\n") == 26 * 3600 + 190);                    // fewer columns, hours > 24
  CHECK(ld::sum_csv_active(std::string(kCsvFile) +
                           "garbage\r\n"
                           ";;;\r\n"
                           "x;y;z;-1:00:00;0:00:00;0\r\n"
                           "x;y;z;1:60:00;0:00:00;0\r\n"
                           "x;y;z;1:00;0:00:00;0\r\n"
                           "x;y;z;:00:00;0:00:00;0\r\n"
                           "x;y;z;1h;0:00:00;0\r\n"
                           "2026 - 10 - 05 - 16:40:30 | 04:09:16\r\n"  // the old StreamDock format
                           "x;y;z;\"0:00:10\";0:00:00;0\r\n") == 14956 + 21720 + 10);
}

namespace {
std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

ld::CsvRow sample_row(long long active_s) {
  ld::CsvRow r;
  r.start.tm_year = 126; r.start.tm_mon = 9; r.start.tm_mday = 15; r.start.tm_hour = 9;
  r.end.tm_hour = 10;
  r.active_s = active_s;
  return r;
}

fs::path fresh_dir(const char* name) {
  fs::path dir = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

#define BOM_HEADER "\xEF\xBB\xBF" "date;start;end;active;paused;pauses\r\n"
}  // namespace

TEST_CASE("append_csv_row creates the file with a BOM and the header, then only appends") {
  const fs::path dir = fresh_dir("ld_sw_append");
  const fs::path f = dir / "log.csv";
  CHECK(ld::append_csv_row(f, sample_row(60)) == 0);
  CHECK(read_all(f) == BOM_HEADER "2026-10-15;09:00:00;10:00:00;0:01:00;0:00:00;0\r\n");
  CHECK(ld::append_csv_row(f, sample_row(120)) == 0);
  CHECK(read_all(f) == BOM_HEADER "2026-10-15;09:00:00;10:00:00;0:01:00;0:00:00;0\r\n"
                                  "2026-10-15;09:00:00;10:00:00;0:02:00;0:00:00;0\r\n");
  { std::ofstream(dir / "empty.csv", std::ios::binary); }  // an empty file gets the header too
  CHECK(ld::append_csv_row(dir / "empty.csv", sample_row(1)) == 0);
  CHECK(read_all(dir / "empty.csv").starts_with(BOM_HEADER));
  { std::ofstream(dir / "edited.csv", std::ios::binary) << "date;start;end;active;paused;pauses\nx;y;z;1:00:00"; }
  CHECK(ld::append_csv_row(dir / "edited.csv", sample_row(1)) == 0);  // no final newline: not glued to it
  CHECK(read_all(dir / "edited.csv") ==
        "date;start;end;active;paused;pauses\nx;y;z;1:00:00\r\n2026-10-15;09:00:00;10:00:00;0:00:01;0:00:00;0\r\n");
  fs::remove_all(dir);
}

TEST_CASE("append_csv_row never creates the folder and never touches a locked file") {
  const fs::path dir = fresh_dir("ld_sw_append_err");
  CHECK(ld::append_csv_row(dir / "missing" / "log.csv", sample_row(1)) == ERROR_PATH_NOT_FOUND);
  CHECK_FALSE(fs::exists(dir / "missing"));
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  HANDLE h = CreateFileW((dir / "log.csv").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  CHECK(ld::append_csv_row(dir / "log.csv", sample_row(1)) == ERROR_SHARING_VIOLATION);
  CloseHandle(h);
  CHECK(read_all(dir / "log.csv") == kCsvFile);
  fs::remove_all(dir);
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

sc::time_point local_time(int y, int mo, int d, int h, int mi, int sec) {
  std::tm t{};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = sec;
  t.tm_isdst = -1;
  return sc::from_time_t(std::mktime(&t));
}

long long to_ms(sc::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
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
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.csv");
  m->on_event({ld::EventKind::Tap});  // start
  CHECK(ctx.state["running"] == true);
  clock.t += 3661s;
  m->on_event({ld::EventKind::LongPress});  // save 1:01:01 and stop at zero
  std::string content = read(dir / "log.csv");
  CHECK(content.starts_with(kCsvFile));  // appended, never rewritten
  CHECK(content.find(";1:01:01;0:00:00;0\r\n") == std::strlen(kCsvFile) + 28);
  CHECK(ld::sum_csv_active(content) == 14956 + 21720 + 3661);
  CHECK(ctx.state["running"] == false);
  CHECK(ctx.state["accumulated_ms"] == 0);
  CHECK(ctx.state["session_start_ms"] == 0);
  CHECK(fixtures::element_of(*m, "label")->text == L"✓ saved");
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
  fs::path file = fs::temp_directory_path() / "ld_sw_restore.csv";
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
  fs::path bad = fs::temp_directory_path() / "ld_no_such_dir" / "sub" / "log.csv";
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
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = start(ctx, dir / "log.csv");
  int before = ctx.invalidations;
  m->on_event({ld::EventKind::LongPress});
  CHECK(ctx.invalidations == before + 1);
  CHECK(render_lit_pixels(*m, r) > 20);
  CHECK(read(dir / "log.csv") == kCsvFile);  // nothing written
  loop.advance(5100ms);
  CHECK(ctx.invalidations == before + 2);  // back to the normal view
  fs::remove_all(dir);
}

TEST_CASE("stopwatch long press at zero shows Error, not a zero total, when the log cannot be read") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_total_locked";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.csv");
  HANDLE h = CreateFileW((dir / "log.csv").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  m->on_event({ld::EventKind::LongPress});
  CloseHandle(h);
  const auto label = fixtures::element_of(*m, "label");
  REQUIRE(label);
  CHECK(label->text == L"Error");
  fs::remove_all(dir);
}

TEST_CASE("stopwatch_file defaults to Documents") {
  CHECK(ld::stopwatch_file("").filename() == "stopwatch_log.csv");
  CHECK(ld::stopwatch_file("C:\\x\\y.txt") == fs::path("C:\\x\\y.txt"));
}

TEST_CASE("stopwatch: a log open in Excel shows Error, keeps the session, and a retry saves it") {
  Clock clock;
  clock.t = local_time(2026, 10, 15, 9, 0, 0);
  const fs::path dir = fresh_dir("ld_sw_locked");
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.csv");
  m->on_event({ld::EventKind::Tap});
  clock.t += 30s;
  HANDLE h = CreateFileW((dir / "log.csv").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  CHECK_NOTHROW(m->on_event({ld::EventKind::LongPress}));
  CloseHandle(h);
  CHECK(fixtures::element_of(*m, "label")->text == L"Error");
  CHECK(read(dir / "log.csv") == kCsvFile);
  CHECK(ctx.state["running"] == true);  // not reset
  CHECK(ctx.state["session_start_ms"] == to_ms(local_time(2026, 10, 15, 9, 0, 0)));
  clock.t += 10s;
  m->on_event({ld::EventKind::LongPress});  // retry, the file closed
  CHECK(read(dir / "log.csv") == std::string(kCsvFile) + "2026-10-15;09:00:00;09:00:40;0:00:40;0:00:00;0\r\n");
  CHECK(ctx.state["running"] == false);
  fs::remove_all(dir);
}

TEST_CASE("stopwatch csv: start, pauses and paused time of the session") {
  Clock clock;
  clock.t = local_time(2026, 10, 15, 9, 0, 0);
  const fs::path dir = fresh_dir("ld_sw_session");
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.csv");  // no file yet: created with its header
  m->on_event({ld::EventKind::Tap});
  clock.t += 1h;
  m->on_event({ld::EventKind::Tap});  // pause 1
  clock.t += 15min;
  m->on_event({ld::EventKind::Tap});
  clock.t += 30min;
  m->on_event({ld::EventKind::Tap});  // pause 2, saved while paused: its time so far counts
  clock.t += 5min;
  m->on_event({ld::EventKind::LongPress});
  CHECK(read(dir / "log.csv") ==
        "\xEF\xBB\xBF" "date;start;end;active;paused;pauses\r\n2026-10-15;09:00:00;10:50:00;1:30:00;0:20:00;2\r\n");
  // A new session starts from scratch.
  m->on_event({ld::EventKind::Tap});
  clock.t += 2min;
  m->on_event({ld::EventKind::LongPress});
  CHECK(read(dir / "log.csv").ends_with("\r\n2026-10-15;10:50:00;10:52:00;0:02:00;0:00:00;0\r\n"));
  fs::remove_all(dir);
}

TEST_CASE("stopwatch csv: a session past midnight is dated by its start, hours beyond 24") {
  Clock clock;
  clock.t = local_time(2026, 10, 15, 23, 30, 0);
  const fs::path dir = fresh_dir("ld_sw_midnight");
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.csv");
  m->on_event({ld::EventKind::Tap});
  clock.t += 26h + 3min + 10s;
  m->on_event({ld::EventKind::LongPress});
  CHECK(read(dir / "log.csv").ends_with("\r\n2026-10-15;23:30:00;01:33:10;26:03:10;0:00:00;0\r\n"));
  fs::remove_all(dir);
}

TEST_CASE("stopwatch csv: restarts keep the session; downtime counts as it was (paused or running)") {
  Clock clock;
  clock.t = local_time(2026, 10, 15, 9, 0, 0);
  const fs::path dir = fresh_dir("ld_sw_restart");
  FakeLoop loop;
  FakeContext ctx(loop);
  {
    auto m = start(ctx, dir / "log.csv");
    m->on_event({ld::EventKind::Tap});
    clock.t += 10min;
    m->on_event({ld::EventKind::Tap});  // paused, then the daemon stops
  }
  clock.t += 20min;  // down while paused: paused time
  {
    auto m = start(ctx, dir / "log.csv");
    clock.t += 5min;
    m->on_event({ld::EventKind::Tap});  // resume, then the daemon stops
  }
  clock.t += 40min;  // down while running: counted time
  {
    auto m = start(ctx, dir / "log.csv");  // a restart does not inflate anything
    auto again = start(ctx, dir / "log.csv");
    clock.t += 5min;
    again->on_event({ld::EventKind::LongPress});
  }
  CHECK(read(dir / "log.csv").ends_with("\r\n2026-10-15;09:00:00;10:20:00;0:55:00;0:25:00;1\r\n"));
  fs::remove_all(dir);
}

TEST_CASE("stopwatch csv: a state from before the CSV log starts its session when its time began") {
  Clock clock;
  clock.t = local_time(2026, 10, 15, 12, 0, 0);
  const fs::path dir = fresh_dir("ld_sw_old_state");
  FakeLoop loop;
  FakeContext ctx(loop);
  ctx.state = {{"running", false}, {"started_ms", 0}, {"accumulated_ms", 3600000}};
  auto m = start(ctx, dir / "log.csv");
  clock.t += 10min;
  m->on_event({ld::EventKind::LongPress});
  CHECK(read(dir / "log.csv").ends_with("\r\n2026-10-15;11:00:00;12:10:00;1:00:00;0:10:00;0\r\n"));
  FakeContext running(loop);
  running.state = {{"running", true}, {"started_ms", to_ms(clock.t) - 60000}, {"accumulated_ms", 60000}};
  auto r = start(running, dir / "log.csv");
  r->on_event({ld::EventKind::LongPress});
  CHECK(read(dir / "log.csv").ends_with("\r\n2026-10-15;12:08:00;12:10:00;0:02:00;0:00:00;0\r\n"));
  fs::remove_all(dir);
}

TEST_CASE("stopwatch csv: a missing folder is an error, never created, and the session is kept") {
  Clock clock;
  const fs::path dir = fresh_dir("ld_sw_nofolder");
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "missing" / "log.csv");
  m->on_event({ld::EventKind::Tap});
  clock.t += 5s;
  m->on_event({ld::EventKind::LongPress});
  CHECK(fixtures::element_of(*m, "label")->text == L"Error");
  CHECK_FALSE(fs::exists(dir / "missing"));
  CHECK(ctx.state["running"] == true);
  CHECK(ctx.state["session_start_ms"] != 0);
  fs::remove_all(dir);
}

TEST_CASE("stopwatch error path handles non-ASCII paths") {
  Clock clock;
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  fs::path bad = fs::temp_directory_path() / L"ld_no_é_日本" / "log.csv";
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
    auto m = start(ctx, fs::temp_directory_path() / "ld_sw_future.csv");
    m->on_event({ld::EventKind::Tap});  // pause
    CHECK(ctx.state["accumulated_ms"] == 2000);
  }
  {
    FakeLoop loop;
    FakeContext ctx(loop);
    ctx.state = {{"running", "yes"}, {"started_ms", "x"}};
    std::unique_ptr<ld::Module> m;
    CHECK_NOTHROW(m = start(ctx, fs::temp_directory_path() / "ld_sw_bad.csv"));
    m->on_event({ld::EventKind::Tap});
    CHECK(ctx.state["running"] == true);
  }
}

TEST_CASE("stopwatch tap clears the total overlay") {
  Clock clock;
  fs::path dir = fs::temp_directory_path() / "ld_sw_tapov";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx, dir / "log.csv");
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
  auto m = start(ctx, dir / "log.csv");
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
  auto m = start(ctx, dir / "log.csv", true);
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
  { std::ofstream(dir / "log.csv", std::ios::binary) << kCsvFile; }
  FakeLoop loop;
  FakeContext ctx(loop);
  auto below = start(ctx, dir / "log.csv", true);
  below->on_event({ld::EventKind::LongPress});
  CHECK(shown(*below) == Shown{{"label", L"Total"}, {"hours_minutes", L"10:11"}, {"seconds", L"16"}});
  FakeContext ctx2(loop);
  auto line = start(ctx2, dir / "log.csv");
  CHECK(shown(*line) == Shown{{"time", L"00:00:00"}});
  line->on_event({ld::EventKind::LongPress});
  CHECK(shown(*line) == Shown{{"label", L"Total"}, {"time", L"10:11:16"}});
  fs::remove_all(dir);
}
