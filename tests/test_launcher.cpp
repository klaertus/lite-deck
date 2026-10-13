// tests/test_launcher.cpp: the launcher key, with a fake launcher and fake windows. Nothing is
// started and no window is touched.
#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "action_fakes.h"
#include "core/log.h"
#include "module_fixtures.h"
#include "modules/launcher/launcher.h"

using nlohmann::json;

namespace {

struct Key {
  std::unique_ptr<fixtures::Rig> rig = std::make_unique<fixtures::Rig>();
  std::shared_ptr<fakes::Launcher> launcher = std::make_shared<fakes::Launcher>();
  fakes::HeldRunner held;
  void press() { rig->m->on_event(ld::InputEvent{ld::EventKind::Press}); }
  // Runs the launches in flight and their results.
  void finish() {
    held.run_all();
    rig->loop.drain();
  }
  ld::ActionKey& key() { return dynamic_cast<ld::ActionKey&>(*rig->m); }
};

Key start(const json& settings) {
  Key k;
  k.rig->start(ld::launcher_module_info(),
               ld::make_launcher_module(k.launcher, ld::ActionDeps{.icons = std::make_shared<fakes::IconLoader>(),
                                                                   .icon_runner = fakes::inline_runner(),
                                                                   .runner = k.held.runner()}),
               settings);
  return k;
}

}  // namespace

TEST_CASE("launcher: sound elements and the schema's defaults") {
  const auto info = ld::launcher_module_info();
  CHECK(ld::element_spec_errors(info.elements).empty());
  REQUIRE(info.elements.size() == 3);
  CHECK(info.elements[0].kind == ld::ElementKind::Icon);
  CHECK(info.elements[2].color == ld::kSecondaryGrey);  // the label is grey
  json s;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, json::object(), s));
  CHECK(s["kind"] == "program");
  CHECK(s["path"] == "");
  CHECK(s["focus_existing"] == false);
  CHECK(s["shell"] == "cmd");
  CHECK(s["window"] == "visible");
  CHECK(s["icon_source"] == "program");
  CHECK(s["glyph"] == "\xF0\x9F\x9A\x80");
  CHECK(s["label"] == "");
  CHECK(ld::apply_schema(info.settings_schema, {{"kind", "run"}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"shell", "bash"}}, s).has_value());
  CHECK_FALSE(info.check);  // nothing beyond the schema: an empty path is a hint on the key
}

TEST_CASE("launch_label, program_command_line and shell_command_line") {
  using ld::LaunchKind;
  CHECK(ld::launch_label({.kind = LaunchKind::Program, .path = L"C:\\Program Files\\App\\Studio.exe"}) == L"Studio");
  CHECK(ld::launch_label({.kind = LaunchKind::Open, .path = L"https://www.github.com/x?y"}) == L"github.com");
  CHECK(ld::launch_label({.kind = LaunchKind::Open, .path = L"http://user:pw@nas.local:5000/"}) == L"nas.local");
  CHECK(ld::launch_label({.kind = LaunchKind::Open, .path = L"mailto:me@example.com"}) == L"mailto");
  CHECK(ld::launch_label({.kind = LaunchKind::Open, .path = L"D:\\Music\\"}) == L"Music");
  CHECK(ld::launch_label({.kind = LaunchKind::Open, .path = L"D:\\Docs\\report.pdf"}) == L"report");
  CHECK(ld::launch_label({.kind = LaunchKind::Command, .shell = ld::Shell::PowerShell}) == L"PowerShell");
  CHECK(ld::program_command_line(L"C:\\A B\\app.exe", L"--x \"y z\"") == L"\"C:\\A B\\app.exe\" --x \"y z\"");
  CHECK(ld::program_command_line(L"app.exe", L"") == L"\"app.exe\"");
  CHECK(ld::shell_command_line(ld::Shell::Cmd, L"echo \"hi\" & pause") == L"cmd.exe /d /s /c \"echo \"hi\" & pause\"");
  // "dir" in UTF-16LE is 64 00 69 00 72 00.
  CHECK(ld::shell_command_line(ld::Shell::PowerShell, L"dir") ==
        L"powershell.exe -NoLogo -NoProfile -EncodedCommand ZABpAHIA");
}

TEST_CASE("launcher: a program with its arguments and folder, the path's variables expanded") {
  REQUIRE(SetEnvironmentVariableW(L"LD_LAUNCH_TEST", L"D:\\Tools"));
  auto k = start({{"path", "%LD_LAUNCH_TEST%\\tool.exe"},
                  {"args", "--open \"%LD_LAUNCH_TEST%\" -v"},
                  {"working_dir", "%LD_LAUNCH_TEST%\\work"}});
  SetEnvironmentVariableW(L"LD_LAUNCH_TEST", nullptr);
  CHECK(fixtures::element_of(*k.rig->m, "label")->text == L"tool");
  k.press();
  CHECK(k.key().flashing());
  CHECK(k.launcher->launched.empty());  // on the worker, not on the loop thread
  k.finish();
  REQUIRE(k.launcher->launched.size() == 1);
  const ld::LaunchRequest& r = k.launcher->launched[0];
  CHECK(r.kind == ld::LaunchKind::Program);
  CHECK(r.path == L"D:\\Tools\\tool.exe");
  CHECK(r.args == L"--open \"%LD_LAUNCH_TEST%\" -v");  // arguments as they are
  CHECK(r.working_dir == L"D:\\Tools\\work");
  CHECK(k.launcher->focus_asked.empty());  // focus_existing is off
  CHECK_FALSE(k.key().failing());
}

TEST_CASE("launcher: open a URL, and a command through its shell") {
  auto url = start({{"kind", "open"}, {"target", "https://example.com"}});
  url.press();
  url.finish();
  REQUIRE(url.launcher->launched.size() == 1);
  CHECK(url.launcher->launched[0] == ld::LaunchRequest{.kind = ld::LaunchKind::Open, .path = L"https://example.com"});
  auto cmd = start({{"kind", "command"}, {"command_line", "ipconfig /flushdns"}, {"shell", "powershell"},
                    {"window", "hidden"}});
  CHECK(fixtures::element_of(*cmd.rig->m, "label")->text == L"PowerShell");
  cmd.press();
  cmd.finish();
  REQUIRE(cmd.launcher->launched.size() == 1);
  const ld::LaunchRequest& r = cmd.launcher->launched[0];
  CHECK(r.kind == ld::LaunchKind::Command);
  CHECK(r.command == L"ipconfig /flushdns");
  CHECK(r.shell == ld::Shell::PowerShell);
  CHECK(r.hidden);
}

TEST_CASE("launcher: not configured, the key says what is missing and a press does nothing") {
  for (const auto& [settings, hint] : {std::pair{json::object(), L"Path?"},
                                       std::pair{json{{"kind", "open"}}, L"Target?"},
                                       std::pair{json{{"kind", "command"}, {"command_line", "  "}}, L"Command?"}}) {
    auto k = start(settings);
    auto title = fixtures::element_of(*k.rig->m, "title");
    REQUIRE(title);
    CHECK(title->text == hint);
    CHECK(title->state == ld::ElementState::Idle);
    k.press();
    k.finish();
    CHECK(k.launcher->launched.empty());
    CHECK_FALSE(k.key().flashing());
  }
}

TEST_CASE("launcher: a press during a launch in flight is ignored") {
  auto k = start({{"path", "C:\\x.exe"}});
  k.press();
  k.press();
  k.press();
  CHECK(k.held.jobs->size() == 1);
  k.finish();
  CHECK(k.launcher->launched.size() == 1);
  k.press();  // done: the next press launches again
  k.finish();
  CHECK(k.launcher->launched.size() == 2);
}

TEST_CASE("launcher: a failed launch shows the \"!\" and logs the reason, never the arguments") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_launcher_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    auto k = start({{"path", "C:\\Apps\\tool.exe"}, {"args", "--password hunter2"}});
    k.launcher->error = "error 740: the program requires administrator rights";
    k.press();
    k.finish();
    CHECK(k.key().failing());
    k.rig->loop.advance(ld::kFailureShown);
    CHECK_FALSE(k.key().failing());
    auto c = start({{"kind", "command"}, {"command_line", "net user bob s3cret"}});
    c.launcher->error = "error 2";
    c.press();
    c.finish();
    CHECK(c.key().failing());
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("launcher: cannot start program tool.exe: error 740") != std::string::npos);
  CHECK(log.find("launcher: cannot start a cmd command: error 2") != std::string::npos);
  CHECK(log.find("hunter2") == std::string::npos);
  CHECK(log.find("s3cret") == std::string::npos);
  CHECK(log.find("C:\\Apps") == std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("launcher: focus_existing brings the running program forward instead of starting it") {
  auto k = start({{"path", "C:\\Apps\\tool.exe"}, {"focus_existing", true}});
  k.launcher->running = true;
  k.press();
  k.finish();
  CHECK(k.launcher->focus_asked == std::vector<std::wstring>{L"C:\\Apps\\tool.exe"});
  CHECK(k.launcher->launched.empty());
  k.launcher->running = false;  // not running: started
  k.press();
  k.finish();
  CHECK(k.launcher->launched.size() == 1);
  auto doc = start({{"path", "C:\\Docs\\a.pdf"}, {"focus_existing", true}});  // only for a program
  doc.press();
  doc.finish();
  CHECK(doc.launcher->focus_asked.empty());
  CHECK(doc.launcher->launched.size() == 1);
}

TEST_CASE("focus_existing_window: the front visible main window of the same exe, restored if minimized") {
  fakes::Windows ws;
  ws.list = {
      {.id = 1, .exe = L"C:\\Apps\\other.exe", .visible = true, .titled = true},
      {.id = 2, .exe = L"C:\\Apps\\Tool.EXE", .visible = false, .titled = true},              // hidden
      {.id = 3, .exe = L"C:\\Apps\\tool.exe", .visible = true, .owned = true, .titled = true},  // a dialog
      {.id = 4, .exe = L"C:\\Apps\\tool.exe", .visible = true, .titled = false},             // untitled
      {.id = 5, .exe = L"C:\\apps\\TOOL.exe", .visible = true, .minimized = true, .titled = true},
      {.id = 6, .exe = L"C:\\Apps\\tool.exe", .visible = true, .titled = true},
  };
  CHECK(ld::focus_existing_window(ws, L"C:\\Apps\\tool.exe"));
  REQUIRE(ws.activated.size() == 1);
  CHECK(ws.activated[0] == std::pair<std::uintptr_t, bool>{5, true});
  ws.activated.clear();
  CHECK(ld::focus_existing_window(ws, L"other.exe"));  // a bare name: by file name
  CHECK(ws.activated == std::vector<std::pair<std::uintptr_t, bool>>{{1, false}});
  ws.activated.clear();
  CHECK_FALSE(ld::focus_existing_window(ws, L"D:\\Apps\\tool.exe"));  // another copy of it
  CHECK_FALSE(ld::focus_existing_window(ws, L"missing.exe"));
  CHECK_FALSE(ld::focus_existing_window(ws, L""));
  CHECK(ws.activated.empty());
}

TEST_CASE("launcher: one pair of surrounding quotes is removed from the paths (Copy as path)") {
  CHECK(ld::unquote(L"  \"C:\\Program Files\\App\\app.exe\" ") == L"C:\\Program Files\\App\\app.exe");
  CHECK(ld::unquote(L"\"\"x\"\"") == L"\"x\"");  // one pair only
  CHECK(ld::unquote(L"\"half") == L"\"half");
  CHECK(ld::unquote(L"\"") == L"\"");
  auto k = start({{"path", "\"C:\\Program Files\\App\\app.exe\""}, {"working_dir", "\"D:\\Work\""},
                  {"focus_existing", true}});
  CHECK(fixtures::element_of(*k.rig->m, "label")->text == L"app");
  k.press();
  k.finish();
  CHECK(k.launcher->focus_asked == std::vector<std::wstring>{L"C:\\Program Files\\App\\app.exe"});  // still an exe
  REQUIRE(k.launcher->launched.size() == 1);
  CHECK(k.launcher->launched[0].path == L"C:\\Program Files\\App\\app.exe");
  CHECK(k.launcher->launched[0].working_dir == L"D:\\Work");
  CHECK(ld::runs_directly(k.launcher->launched[0]));
  auto open = start({{"kind", "open"}, {"target", "\"D:\\Docs\\a.pdf\""}});
  open.press();
  open.finish();
  CHECK(open.launcher->launched.at(0).path == L"D:\\Docs\\a.pdf");
}

TEST_CASE("runs_directly: every exe goes through CreateProcessW, never ShellExecute") {
  using ld::LaunchKind;
  CHECK(ld::runs_directly({.kind = LaunchKind::Program, .path = L"C:\\Apps\\tool.exe"}));
  CHECK(ld::runs_directly({.kind = LaunchKind::Program, .path = L"notepad.EXE"}));
  CHECK(ld::runs_directly({.kind = LaunchKind::Open, .path = L"C:\\Apps\\setup.exe"}));  // open an exe: started too
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Program, .path = L"C:\\Docs\\a.pdf"}));
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Open, .path = L"C:\\Links\\Admin tool.lnk"}));
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Open, .path = L"C:\\x\\setup.msi"}));
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Open, .path = L"compmgmt.msc"}));
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Open, .path = L"D:\\Music\\"}));
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Open, .path = L"https://example.com/app.exe"}));
  CHECK_FALSE(ld::runs_directly({.kind = LaunchKind::Command, .path = L"x.exe", .command = L"x.exe"}));
}

TEST_CASE("launcher: a relative path with a folder asks for a full path") {
  auto k = start({{"path", "tools\\app.exe"}});
  CHECK(fixtures::element_of(*k.rig->m, "title")->text == L"Full path?");
  k.press();
  k.finish();
  CHECK(k.launcher->launched.empty());
  auto open = start({{"kind", "open"}, {"target", "docs/a.pdf"}});
  CHECK(fixtures::element_of(*open.rig->m, "title")->text == L"Full path?");
  for (const char* ok : {"notepad.exe", "C:\\x\\app.exe", "\\\\server\\share\\app.exe"}) {
    auto good = start({{"path", ok}});
    CHECK_FALSE(fixtures::element_of(*good.rig->m, "title"));
  }
}

TEST_CASE("launcher: a job that throws, or never answers, does not leave the key busy") {
  auto k = start({{"path", "C:\\x.exe"}});
  k.launcher->throws = true;
  k.press();
  k.finish();
  CHECK(k.key().failing());
  CHECK_FALSE(k.key().busy());

  auto hung = start({{"path", "C:\\x.exe"}});
  hung.press();  // the job is held: it never answers
  CHECK(hung.key().busy());
  hung.rig->loop.advance(ld::kActionTimeout);
  CHECK_FALSE(hung.key().busy());
  CHECK(hung.key().failing());
  hung.press();  // taken again
  CHECK(hung.held.jobs->size() == 2);
  hung.finish();  // the given-up one is dropped, the current one runs and ends the action
  CHECK_FALSE(hung.key().busy());
  CHECK(hung.launcher->launched.size() == 1);
}

TEST_CASE("launcher: presses queued behind a hung launch are dropped once given up, never run in a burst") {
  auto k = start({{"path", "C:\\x.exe"}});
  for (int i = 0; i < 3; ++i) {  // the worker is stuck: each press waits behind it, then times out
    k.press();
    k.rig->loop.advance(ld::kActionTimeout);
    CHECK_FALSE(k.key().busy());
  }
  k.press();
  CHECK(k.held.jobs->size() == 4);
  k.finish();
  CHECK(k.launcher->launched.size() == 1);  // one start, not four
  CHECK_FALSE(k.key().busy());
}

TEST_CASE("launcher: an open target or a document is logged by its kind, never by its name") {
  CHECK(ld::kind_for_log(L"D:\\HR\\salaries-2026.XLSX") == "a .xlsx file");
  CHECK(ld::kind_for_log(L"D:\\HR\\") == "a folder");
  CHECK(ld::kind_for_log(L"https://intranet/x") == "a https link");
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_launcher_open_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    auto k = start({{"kind", "open"}, {"target", "D:\\HR\\salaries-2026.xlsx"}});
    k.launcher->error = "error 1155";
    k.press();
    k.finish();
    auto doc = start({{"path", "D:\\HR\\review.docx"}});
    doc.launcher->error = "error 2";
    doc.press();
    doc.finish();
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("launcher: cannot start open a .xlsx file: error 1155") != std::string::npos);
  CHECK(log.find("launcher: cannot start open a .docx file: error 2") != std::string::npos);
  CHECK(log.find("salaries") == std::string::npos);
  CHECK(log.find("review") == std::string::npos);
  fs::remove_all(dir);
}
