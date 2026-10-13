#include <doctest/doctest.h>
#include <windows.h>
#include <dbt.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "app/app_window.h"
#include "app/shell.h"
#include "app/paths.h"
#include "app/process.h"
#include "app/single_instance.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>
#include <wtsapi32.h>

namespace {

namespace fs = std::filesystem;

// A fresh base folder standing in for %APPDATA% (the tests never touch the real one).
struct AppDataBase {
  fs::path path;
  explicit AppDataBase(const char* name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~AppDataBase() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_text(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

std::string read_text(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// The former data folder as a user has it: config, state, logs and a file the copy leaves alone.
void make_legacy(const fs::path& base) {
  const fs::path old = base / L"libre-deck";
  write_text(old / "config.json", "{\"config\":1}");
  write_text(old / "state.json", "{\"state\":1}");
  write_text(old / "logs" / "libre-deck.log", "old log");
  write_text(old / "sim" / "frame.png", "png");
}

}  // namespace

TEST_CASE("app_data_dir is <base>/lite-deck and exists") {
  AppDataBase b("ld_appdata_dir");
  const fs::path p = ld::app_data_dir(b.path);
  CHECK(p == b.path / L"lite-deck");
  CHECK(fs::is_directory(p));
}

TEST_CASE("migrate_legacy_data_dir: nothing to do without the old folder") {
  AppDataBase b("ld_appdata_none");
  CHECK(ld::migrate_legacy_data_dir(b.path).kind == ld::DataDirMigration::Kind::None);
  CHECK_FALSE(fs::exists(b.path / L"lite-deck"));
  CHECK(ld::migrate_legacy_data_dir(fs::path()).kind == ld::DataDirMigration::Kind::None);
}

TEST_CASE("migrate_legacy_data_dir: renames the old folder to the new name") {
  AppDataBase b("ld_appdata_move");
  make_legacy(b.path);
  const ld::DataDirMigration m = ld::migrate_legacy_data_dir(b.path);
  CHECK(m.kind == ld::DataDirMigration::Kind::Moved);
  CHECK(m.message.find("libre-deck") != std::string::npos);
  CHECK_FALSE(fs::exists(b.path / L"libre-deck"));
  const fs::path dir = ld::app_data_dir(b.path);
  CHECK(read_text(dir / "config.json") == "{\"config\":1}");
  CHECK(read_text(dir / "state.json") == "{\"state\":1}");
  CHECK(read_text(dir / "logs" / "libre-deck.log") == "old log");
  CHECK(fs::exists(dir / "sim" / "frame.png"));
  CHECK(ld::migrate_legacy_data_dir(b.path).kind == ld::DataDirMigration::Kind::None);  // once
}

TEST_CASE("migrate_legacy_data_dir: an empty new folder does not block the move") {
  AppDataBase b("ld_appdata_empty");
  make_legacy(b.path);
  fs::create_directories(b.path / L"lite-deck");
  CHECK(ld::migrate_legacy_data_dir(b.path).kind == ld::DataDirMigration::Kind::Moved);
  CHECK(read_text(b.path / L"lite-deck" / "config.json") == "{\"config\":1}");
  CHECK_FALSE(fs::exists(b.path / L"libre-deck"));
}

TEST_CASE("migrate_legacy_data_dir: a new folder with data is left alone, and so is the old one") {
  AppDataBase b("ld_appdata_both");
  make_legacy(b.path);
  write_text(b.path / L"lite-deck" / "config.json", "{\"new\":1}");
  CHECK(ld::migrate_legacy_data_dir(b.path).kind == ld::DataDirMigration::Kind::None);
  CHECK(read_text(b.path / L"lite-deck" / "config.json") == "{\"new\":1}");
  CHECK(read_text(b.path / L"libre-deck" / "config.json") == "{\"config\":1}");
}

TEST_CASE("migrate_legacy_data_dir: a locked old folder is copied (config, state, logs) and kept") {
  AppDataBase b("ld_appdata_locked");
  make_legacy(b.path);
  // An open handle without FILE_SHARE_DELETE makes the folder rename fail, as a running old daemon would.
  const std::wstring locked = (b.path / L"libre-deck" / "sim" / "frame.png").wstring();
  HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  const ld::DataDirMigration m = ld::migrate_legacy_data_dir(b.path);
  CloseHandle(h);
  CHECK(m.kind == ld::DataDirMigration::Kind::Copied);
  const fs::path dir = b.path / L"lite-deck";
  CHECK(read_text(dir / "config.json") == "{\"config\":1}");
  CHECK(read_text(dir / "state.json") == "{\"state\":1}");
  CHECK(read_text(dir / "logs" / "libre-deck.log") == "old log");
  CHECK_FALSE(fs::exists(dir / "sim"));  // only the three items
  CHECK(fs::exists(b.path / L"libre-deck" / "config.json"));  // the old folder stays
  CHECK(ld::migrate_legacy_data_dir(b.path).kind == ld::DataDirMigration::Kind::None);  // once
}

TEST_CASE("process_running finds the test runner itself and not a fake name") {
  CHECK(ld::process_running(L"lite-deck-tests.exe"));
  CHECK_FALSE(ld::process_running(L"definitely-not-running-xyz.exe"));
}

#include "app/power_state.h"

TEST_CASE("PowerState: lock, suspend, resume, unlock wakes only at unlock") {
  ld::PowerState p;
  CHECK(p.on_lock());
  CHECK_FALSE(p.on_suspend());
  CHECK_FALSE(p.on_resume());
  CHECK(p.on_unlock());
}

TEST_CASE("PowerState: suspend and resume without lock wakes at resume") {
  ld::PowerState p;
  CHECK(p.on_suspend());
  CHECK(p.on_resume());
  CHECK_FALSE(p.on_unlock());  // spurious unlock while awake does nothing
}

TEST_CASE("WM_COMMAND kCmdConfigure opens the page only while the web UI is on") {
  int opened = 0;
  ld::AppWindow w(GetModuleHandleW(nullptr), ld::AppWindow::Callbacks{.on_configure = [&] { ++opened; }});
  REQUIRE(w.hwnd());
  SendMessageW(w.hwnd(), WM_COMMAND, ld::kCmdConfigure, 0);
  CHECK(opened == 0);  // disabled until the server listens
  w.set_configure_enabled(true);
  SendMessageW(w.hwnd(), WM_COMMAND, ld::kCmdConfigure, 0);
  CHECK(opened == 1);
  wchar_t cls[64]{};
  GetClassNameW(w.hwnd(), cls, 64);
  CHECK(std::wstring(cls) == ld::kAppWindowClass);  // what a second instance looks for
  CHECK_NOTHROW(w.set_alert(L"config.json is invalid"));
  CHECK_NOTHROW(w.set_alert(L""));
}

TEST_CASE("open_url refuses an empty URL") {
  CHECK_FALSE(ld::open_url(L""));
}

// A class name of our own, so the tests never talk to a real lite-deck that may be running.
static constexpr wchar_t kTestClass[] = L"LiteDeckHiddenWindowTest";

static void pump_messages() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

TEST_CASE("AppWindow: WM_CLOSE asks to quit instead of destroying the window") {
  int quits = 0;
  ld::AppWindow w(GetModuleHandleW(nullptr), ld::AppWindow::Callbacks{.on_quit = [&] { ++quits; }}, kTestClass);
  SendMessageW(w.hwnd(), WM_CLOSE, 0, 0);
  CHECK(quits == 1);
  CHECK(IsWindow(w.hwnd()));
}

static bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    pump_messages();
    Sleep(5);
  }
  return true;
}

TEST_CASE("AppWindow: the tray menu runs on its own thread; its choice comes back on the window's thread") {
  int quits = 0;
  DWORD quit_thread = 0;
  ld::AppWindow w(GetModuleHandleW(nullptr), ld::AppWindow::Callbacks{.on_quit = [&] {
                    ++quits;
                    quit_thread = GetCurrentThreadId();
                  }},
                  kTestClass);
  HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<DWORD> menu_thread{0};
  std::atomic<int> items{0};
  w.set_menu_tracker([&](HMENU menu, HWND) -> UINT {
    menu_thread = GetCurrentThreadId();
    items = GetMenuItemCount(menu);
    SetEvent(entered);
    WaitForSingleObject(release, 5000);  // the user takes their time
    return ld::kCmdQuit;
  });
  const auto t0 = std::chrono::steady_clock::now();
  SendMessageW(w.hwnd(), ld::kTrayMessage, 0, WM_RBUTTONUP);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));  // not blocked by the menu
  REQUIRE(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0);
  CHECK(menu_thread != GetCurrentThreadId());
  CHECK(items == 4);  // status, separator, Configure…, Quit
  SendMessageW(w.hwnd(), ld::kTrayMessage, 0, WM_RBUTTONUP);  // a second click while open: no second menu
  pump_messages();
  CHECK(quits == 0);
  SetEvent(release);
  CHECK(pump_until([&] { return quits > 0; }, std::chrono::milliseconds(2000)));
  CHECK(quits == 1);
  CHECK(quit_thread == GetCurrentThreadId());
  CloseHandle(entered);
  CloseHandle(release);
}

TEST_CASE("AppWindow: a callback that throws is logged, never propagated") {
  ld::AppWindow w(GetModuleHandleW(nullptr),
                  ld::AppWindow::Callbacks{.on_quit = [] { throw std::runtime_error("quit failed"); },
                                           .on_lock = [] { throw 42; },
                                           .on_configure = [] { throw std::runtime_error("configure failed"); }},
                  kTestClass);
  w.set_configure_enabled(true);
  CHECK_NOTHROW(SendMessageW(w.hwnd(), WM_CLOSE, 0, 0));
  CHECK_NOTHROW(SendMessageW(w.hwnd(), WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0));
  CHECK_NOTHROW(SendMessageW(w.hwnd(), WM_COMMAND, ld::kCmdConfigure, 0));
  PostMessageW(w.hwnd(), WM_CLOSE, 0, 0);
  CHECK_NOTHROW(pump_messages());
  CHECK(IsWindow(w.hwnd()));
}

TEST_CASE("AppWindow: destroying the window closes an open tray menu") {
  // A real TrackPopupMenu, on a thread of its own so a hang fails the test instead of blocking it.
  std::atomic<bool> done{false};
  std::thread t([&] {
    ld::AppWindow w(GetModuleHandleW(nullptr), ld::AppWindow::Callbacks{}, kTestClass);
    SendMessageW(w.hwnd(), ld::kTrayMessage, 0, WM_RBUTTONUP);
    Sleep(300);  // the menu is up (or failed to show, without a desktop)
    // ~AppWindow cancels it
  });
  std::thread waiter([&] {
    t.join();
    done = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done && std::chrono::steady_clock::now() < deadline) Sleep(10);
  CHECK(done.load());
  if (done) waiter.join();
  else waiter.detach();  // leaked on failure: better than hanging the whole run
}

TEST_CASE("signal_running_instance makes the running window open its config page") {
  int opened = 0;
  ld::AppWindow w(GetModuleHandleW(nullptr), ld::AppWindow::Callbacks{.on_configure = [&] { ++opened; }}, kTestClass);
  w.set_configure_enabled(true);
  CHECK(ld::signal_running_instance(kTestClass, ld::Duration(1000)));
  pump_messages();
  CHECK(opened == 1);
}

TEST_CASE("signal_running_instance gives up when no instance is running") {
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(ld::signal_running_instance(L"LiteDeckNoSuchWindowClass", ld::Duration(300)));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

TEST_CASE("AppWindow: a device arrival reaches the app only when accepts_device takes its path") {
  int arrivals = 0;
  ld::AppWindow w(GetModuleHandleW(nullptr),
                  ld::AppWindow::Callbacks{.on_device_arrival = [&] { ++arrivals; },
                                           .accepts_device = [](std::wstring_view p) {
                                             return p.find(L"VID_FFFF&PID_FFFE") != std::wstring_view::npos;
                                           }},
                  kTestClass);
  auto arrive = [&](const std::wstring& path) {
    std::vector<std::uint8_t> buf(sizeof(DEV_BROADCAST_DEVICEINTERFACE_W) + (path.size() + 1) * sizeof(wchar_t), 0);
    auto* d = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(buf.data());
    d->dbcc_size = static_cast<DWORD>(buf.size());
    d->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    wmemcpy(d->dbcc_name, path.c_str(), path.size() + 1);
    SendMessageW(w.hwnd(), WM_DEVICECHANGE, DBT_DEVICEARRIVAL, reinterpret_cast<LPARAM>(buf.data()));
  };
  arrive(L"\\\\?\\HID#VID_FFFF&PID_FFFE#x");
  arrive(L"\\\\?\\HID#VID_0781&PID_5567#x");
  CHECK(arrivals == 1);
}
