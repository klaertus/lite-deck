// tests/test_tap_action.cpp: the tap action of the monitor keys, with a fake launcher. Nothing is
// started.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "action_fakes.h"
#include "core/log.h"
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/tap_action.h"
#include "modules/schema.h"

using nlohmann::json;

namespace {

// Installs the fake launcher and a held runner for the tap actions started while it lives.
struct TapRig {
  std::shared_ptr<fakes::Launcher> launcher = std::make_shared<fakes::Launcher>();
  fakes::HeldRunner held;
  std::unique_ptr<fixtures::Rig> rig = std::make_unique<fixtures::Rig>();
  TapRig() { ld::set_tap_deps(ld::TapDeps{.launcher = launcher, .runner = held.runner()}); }
  ~TapRig() { ld::set_tap_deps({}); }
  TapRig(const TapRig&) = delete;
  TapRig& operator=(const TapRig&) = delete;
  void start(const ld::ModuleInfo& info, std::unique_ptr<ld::Module> m, const json& over = json::object()) {
    rig->start(info, std::move(m), over);
  }
  void tap() { rig->m->on_event(ld::InputEvent{ld::EventKind::Tap}); }
  void finish() {  // runs the launches in flight and their results
    held.run_all();
    rig->loop.drain();
  }
  const ld::ActionFeedback& fb() { return dynamic_cast<ld::TapKey&>(*rig->m).tap_feedback(); }
};

ld::Runner dropping_runner() {
  return [](std::function<void()>) {};
}

struct Monitor {
  const char* name;
  std::function<ld::ModuleInfo()> info;
  std::function<std::unique_ptr<ld::Module>()> make;
  json over = json::object();
};

std::vector<Monitor> monitors() {
  auto no_temp = [] { return std::optional<double>{}; };
  return {
      {"cpu_load", ld::cpu_load_module_info, [=] { return ld::make_cpu_load_module(no_temp); }},
      {"cpu_temp", ld::cpu_temp_module_info, [=] { return ld::make_cpu_temp_module(no_temp); }},
      {"ram", ld::ram_module_info,
       [] { return ld::make_ram_module([]() -> std::optional<std::pair<std::uint64_t, std::uint64_t>> { return std::nullopt; }); }},
      {"gpu vram", ld::gpu_module_info, [] { return ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; }); },
       {{"mode", "vram"}}},
      {"gpu temp", ld::gpu_module_info, [] { return ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; }); },
       {{"mode", "temp"}}},
      {"gpu usage", ld::gpu_module_info,
       [] {
         return ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; }, [] { return ld::GpuUsageSample{}; });
       },
       {{"mode", "usage"}}},
      {"net_speed", ld::net_speed_module_info,
       [] { return ld::make_net_speed_module([] { return std::optional<std::vector<ld::NetCounter>>{}; }); }},
      {"disk space", ld::disk_module_info,
       [] {
         return ld::make_disk_module([](const std::wstring&) { return std::optional<ld::DiskSpace>{}; },
                                     [] { return std::optional<ld::DiskIo>{}; }, dropping_runner());
       },
       {{"mode", "space"}}},
      {"disk io", ld::disk_module_info,
       [] {
         return ld::make_disk_module([](const std::wstring&) { return std::optional<ld::DiskSpace>{}; },
                                     [] { return std::optional<ld::DiskIo>{}; }, dropping_runner());
       },
       {{"mode", "io"}}},
      {"ping", ld::ping_module_info,
       [] {
         return ld::make_ping_module([](const std::string&) { return std::optional<std::string>{}; },
                                     [](const std::string&, int) { return std::optional<int>{}; }, dropping_runner());
       }},
  };
}

}  // namespace

TEST_CASE("tap action: the schema's defaults of each module, in the page's tap group") {
  struct Want {
    ld::ModuleInfo info;
    const char* action;
    const char* target;
  };
  for (const Want& w : {Want{ld::cpu_load_module_info(), "task_manager", ""}, Want{ld::cpu_temp_module_info(), "task_manager", ""},
                        Want{ld::ram_module_info(), "task_manager", ""}, Want{ld::gpu_module_info(), "task_manager", ""},
                        Want{ld::net_speed_module_info(), "task_manager", ""}, Want{ld::disk_module_info(), "task_manager", ""},
                        Want{ld::ping_module_info(), "task_manager", ""},
                        Want{ld::weather_module_info(), "open", "https://open-meteo.com"},
                        Want{ld::date_module_info(), "none", ""}, Want{ld::clock_module_info(), "open", "ms-clock:"}}) {
    INFO(w.info.id);
    json s;
    REQUIRE_FALSE(ld::apply_schema(w.info.settings_schema, json::object(), s));
    CHECK(s["tap_action"] == w.action);
    CHECK(s["tap_target"] == w.target);
    for (const json& f : w.info.settings_schema["fields"])
      if (f["key"].get<std::string>().starts_with("tap_")) CHECK(f["group"] == "tap");
    CHECK(ld::apply_schema(w.info.settings_schema, {{"tap_action", "run"}}, s).has_value());
  }
  // A key whose tap already does something keeps it, without the setting.
  for (const ld::ModuleInfo& info : {ld::stopwatch_module_info(), ld::crypto_gold_module_info()}) {
    INFO(info.id);
    for (const json& f : info.settings_schema["fields"]) CHECK_FALSE(f["key"].get<std::string>().starts_with("tap_"));
  }
}

TEST_CASE("tap action: every monitor key starts Task Manager by default, through the shell, on a tap only") {
  for (const Monitor& m : monitors()) {
    INFO(m.name);
    TapRig t;
    t.start(m.info(), m.make(), m.over);
    t.rig->m->on_event(ld::InputEvent{ld::EventKind::Press});
    t.rig->m->on_event(ld::InputEvent{ld::EventKind::LongPress});
    t.rig->m->on_event(ld::InputEvent{ld::EventKind::Release});
    t.finish();
    CHECK(t.launcher->launched.empty());  // a press or a long press: nothing
    t.tap();
    CHECK(t.fb().flashing());
    CHECK(t.fb().busy());
    t.finish();
    REQUIRE(t.launcher->launched.size() == 1);
    CHECK(t.launcher->launched[0] == ld::task_manager_request());
    CHECK_FALSE(t.fb().busy());
    CHECK_FALSE(t.fb().failing());
  }
  const ld::LaunchRequest tm = ld::task_manager_request();
  CHECK(tm.kind == ld::LaunchKind::Program);
  CHECK(tm.path == ld::system_directory() + L"\\taskmgr.exe");  // never a PATH search
  CHECK(tm.path.substr(1, 2) == L":\\");  // a full path from a drive
  CHECK(tm.via_shell);
  CHECK_FALSE(ld::runs_directly(tm));  // ShellExecuteExW: Windows decides about elevation
}

TEST_CASE("tap action: the weather opens open-meteo.com, the clock the Clock app, the date does nothing") {
  {
    TapRig t;
    t.start(ld::weather_module_info(), ld::weather_module_info().create(), {{"city", "Brussels"}});
    t.tap();
    t.finish();
    REQUIRE(t.launcher->launched.size() == 1);
    CHECK(t.launcher->launched[0] == ld::LaunchRequest{.kind = ld::LaunchKind::Open, .path = L"https://open-meteo.com"});
  }
  {
    TapRig t;
    t.start(ld::clock_module_info(), ld::clock_module_info().create());
    t.tap();
    t.finish();
    REQUIRE(t.launcher->launched.size() == 1);
    CHECK(t.launcher->launched[0] == ld::LaunchRequest{.kind = ld::LaunchKind::Open, .path = L"ms-clock:"});
  }
  {
    TapRig t;
    t.start(ld::date_module_info(), ld::date_module_info().create());
    t.tap();
    CHECK_FALSE(t.fb().flashing());
    t.finish();
    CHECK(t.launcher->launched.empty());
  }
}

TEST_CASE("tap action: none does nothing, not even the flash") {
  TapRig t;
  t.start(ld::cpu_load_module_info(), ld::make_cpu_load_module({}), {{"tap_action", "none"}});
  t.tap();
  CHECK_FALSE(t.fb().flashing());
  CHECK_FALSE(t.fb().busy());
  t.finish();
  CHECK(t.launcher->launched.empty());
  CHECK(t.held.jobs->empty());
}

TEST_CASE("tap action: open routes its target like the launcher's open kind") {
  auto opened = [](const char* target) {
    TapRig t;
    t.start(ld::ram_module_info(), ld::make_ram_module({}), {{"tap_action", "open"}, {"tap_target", target}});
    t.tap();
    t.finish();
    REQUIRE(t.launcher->launched.size() == 1);
    return t.launcher->launched[0];
  };
  const auto url = opened("  \"https://example.com/x?y\"  ");  // trimmed, unquoted
  CHECK(url == ld::LaunchRequest{.kind = ld::LaunchKind::Open, .path = L"https://example.com/x?y"});
  CHECK_FALSE(ld::runs_directly(url));
  const auto exe = opened("\"C:\\Program Files\\App\\app.exe\"");
  CHECK(exe.path == L"C:\\Program Files\\App\\app.exe");
  CHECK(ld::runs_directly(exe));  // an exe: CreateProcessW, never elevated
  CHECK_FALSE(exe.via_shell);
  const auto doc = opened("%SystemRoot%\\win.ini");
  CHECK(doc.path == ld::expand_env(L"%SystemRoot%") + L"\\win.ini");
  CHECK_FALSE(ld::runs_directly(doc));
  CHECK(opened("D:\\Music\\").path == L"D:\\Music\\");
}

TEST_CASE("tap action: open without a usable target shows the \"!\" and starts nothing") {
  for (const char* target : {"", "   ", "tools\\app.exe"}) {
    INFO(target);
    TapRig t;
    t.start(ld::cpu_load_module_info(), ld::make_cpu_load_module({}), {{"tap_action", "open"}, {"tap_target", target}});
    t.tap();
    CHECK(t.fb().failing());
    t.finish();
    CHECK(t.launcher->launched.empty());
    CHECK(t.held.jobs->empty());
  }
}

TEST_CASE("tap action: a failure shows the \"!\" for 1.5 s and logs the kind, never the target") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_tap_action_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    TapRig t;
    t.start(ld::weather_module_info(), ld::weather_module_info().create(),
            {{"tap_target", "https://example.com/private?token=1"}});
    t.launcher->error = "error 2: The system cannot find the file specified";
    t.tap();
    t.finish();
    CHECK(t.fb().failing());
    CHECK_FALSE(t.fb().busy());
    t.rig->loop.advance(ld::kFailureShown);
    CHECK_FALSE(t.fb().failing());
  }
  {
    TapRig t;
    t.start(ld::ram_module_info(), ld::make_ram_module({}));
    t.launcher->throws = true;
    t.tap();
    t.finish();
    CHECK(t.fb().failing());
  }
  {
    TapRig t;
    t.start(ld::ram_module_info(), ld::make_ram_module({}),
            {{"tap_action", "open"}, {"tap_target", "C:\\Secret\\report.xlsx"}});
    t.launcher->error = "error 5: Access is denied";
    t.tap();
    t.finish();
    CHECK(t.fb().failing());
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("tap action: cannot start open a https link: error 2") != std::string::npos);
  CHECK(log.find("tap action: cannot start Task Manager: failed: boom") != std::string::npos);
  CHECK(log.find("tap action: cannot start open a .xlsx file: error 5") != std::string::npos);
  CHECK(log.find("token") == std::string::npos);
  CHECK(log.find("example.com") == std::string::npos);
  CHECK(log.find("Secret") == std::string::npos);
  CHECK(log.find("report") == std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("tap action: taps are ignored while a launch is in flight; a hung one is given up after 30 s") {
  TapRig t;
  t.start(ld::cpu_temp_module_info(), ld::make_cpu_temp_module([] { return std::optional<double>{}; }));
  t.tap();
  t.tap();
  t.tap();
  CHECK(t.held.jobs->size() == 1);
  CHECK(t.fb().busy());
  t.finish();
  CHECK(t.launcher->launched.size() == 1);
  t.tap();  // done: takes taps again
  CHECK(t.held.jobs->size() == 1);
  t.rig->loop.advance(ld::kActionTimeout);  // never answers
  CHECK_FALSE(t.fb().busy());
  CHECK(t.fb().failing());
  t.finish();  // given up before it ran: dropped
  CHECK(t.launcher->launched.size() == 1);
  CHECK_FALSE(t.fb().busy());
  t.rig->loop.advance(ld::kFailureShown);
  CHECK_FALSE(t.fb().failing());
}

TEST_CASE("tap action: taps queued behind a hung launch are dropped once given up, never run in a burst") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_tap_action_hung_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    TapRig t;
    t.start(ld::cpu_load_module_info(), ld::make_cpu_load_module({}));
    for (int i = 0; i < 3; ++i) {  // the worker is stuck: each tap waits behind it, then times out
      t.tap();
      t.rig->loop.advance(ld::kActionTimeout);
      CHECK_FALSE(t.fb().busy());
    }
    t.tap();  // the current one
    CHECK(t.held.jobs->size() == 4);
    t.finish();  // the worker is free again
    CHECK(t.launcher->launched.size() == 1);  // one Task Manager, not four
    CHECK_FALSE(t.fb().busy());
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("tap action: the action did not finish within 30 s") != std::string::npos);
  CHECK(log.find("action key:") == std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("tap action: the flash and the \"!\" are drawn only while they last") {
  fixtures::FrozenWallClock frozen(2026, 10, 14, 13, 37, 42);
  ld::Renderer r;
  TapRig t;
  t.rig = fixtures::clock_rig(true);  // started with the fake launcher: the tap opens ms-clock:
  auto base = draw(*t.rig->m, r);
  check_golden(r, *base, "clock_seconds");  // the key's own image, as before the tap actions
  t.launcher->error = "error 2";
  t.tap();
  t.finish();
  REQUIRE(t.fb().flashing());
  REQUIRE(t.fb().failing());
  auto lit = draw(*t.rig->m, r);
  CHECK(saturated(*lit, 80, 0, 112, 30) > 20);  // the red "!"
  CHECK(saturated(*base, 80, 0, 112, 30) == 0);
  CHECK(lit->pixel(5, 100).r > 50);  // the flash: the black corner turned grey
  t.rig->loop.advance(ld::kPressFlash);
  auto failing = draw(*t.rig->m, r);
  CHECK(failing->pixel(5, 100).r == 0);
  CHECK(saturated(*failing, 80, 0, 112, 30) > 20);
  t.rig->loop.advance(ld::kFailureShown);
  auto after = draw(*t.rig->m, r);
  CHECK(same_rows(*after, *base, 0, 112));
}
