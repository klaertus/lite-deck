// src/main.cpp
#include <windows.h>
#include <shellapi.h>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include "app/app_window.h"
#include "app/paths.h"
#include "app/power_state.h"
#include "app/process.h"
#include "app/shutdown_watchdog.h"
#include "app/single_instance.h"
#include "app/shell.h"
#include "core/config_service.h"
#include "core/config_watcher.h"
#include "core/event_loop.h"
#include "core/http.h"
#include "core/log.h"
#include "core/state_store.h"
#include "core/strings.h"
#include "device/device_manager.h"
#include "device/registry.h"
#include "device/simulator.h"
#include "layout/deck.h"
#include "modules/module.h"
#include "render/renderer.h"
#include "web/web_ui.h"

namespace {

const char* device_state_name(ld::DeviceManager::State s) {
  using S = ld::DeviceManager::State;
  return s == S::Connected ? "connected" : s == S::Busy ? "busy" : "disconnected";
}

// `vendor`: the vendor app as shown (ld::vendor_app_label), e.g. "StreamDock".
std::wstring device_state_text(ld::DeviceManager::State s, const std::wstring& vendor) {
  using S = ld::DeviceManager::State;
  return s == S::Connected ? L"Connected" : s == S::Busy ? vendor + L" active: deck released" : L"Deck disconnected";
}

const wchar_t* kConfigAlert = L"config.json is invalid (see the log)";

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  bool simulate = false;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i)
    if (wcscmp(argv[i], L"--simulate") == 0) simulate = true;
  LocalFree(argv);

  HANDLE mutex = CreateMutexW(nullptr, TRUE, ld::kSingleInstanceMutex);
  if (!mutex) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    ld::signal_running_instance(ld::kAppWindowClass);  // open the running instance's config page
    CloseHandle(mutex);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const auto base = ld::roaming_app_data();
  const ld::DataDirMigration moved = ld::migrate_legacy_data_dir(base);  // before anything opens a file there
  const auto dir = ld::app_data_dir(base);
  const auto log_file = dir / "logs" / "lite-deck.log";
  ld::log_init(log_file, ld::LogLevel::Info);
  ld::log_info("lite-deck starting{}", simulate ? " (simulator)" : "");
  if (moved.kind == ld::DataDirMigration::Kind::Failed) ld::log_warn("{}", moved.message);
  else if (moved.kind != ld::DataDirMigration::Kind::None) ld::log_info("{}", moved.message);

  // Armed when the exit starts (Quit, WM_CLOSE from an installer, the loop ending): the process is
  // ended if the exit takes more than kShutdownBudget. Never destroyed, so that it also covers the
  // CRT's exit after wWinMain returns (atexit handlers, static destructors). It cannot cover DLL
  // detach: ExitProcess ends every other thread, the watchdog's too, before that.
  auto* watchdog = new ld::ShutdownWatchdog(ld::end_process_now);
  int code = 0;
  bool running = false;  // past startup: a fatal error before this is shown to the user
  auto fatal = [&](const std::string& what) {
    ld::log_error("fatal: {}", what);
    code = 1;
    if (running) return;
    const std::wstring text = L"lite-deck could not start:\n\n" + ld::widen(what) + L"\n\nLog: " + log_file.wstring();
    MessageBoxW(nullptr, text.c_str(), L"lite-deck", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  };
  try {
    const auto config_file = dir / "config.json";
    // The active device (spec §3): plugged in, else the one config.json names, else the N4 Pro. Fixed
    // until the next start: it sizes the config, the deck and the page.
    const ld::DeviceEntry& active =
        ld::choose_device(ld::device_registry(), ld::hid_present, ld::configured_device(config_file));
    ld::log_info("device: {}", active.layout.name);
    const std::wstring vendor = ld::widen(ld::vendor_app_label(active.layout));  // shown while it holds the device
    ld::ConfigService config(config_file, ld::find_module, ld::config_shape(active.layout));
    const ld::Config initial = config.load();
    ld::Renderer renderer;
    ld::EventLoop loop;

    std::unique_ptr<ld::DeviceManager> device;
    std::unique_ptr<ld::SimulatorSink> sim_sink;
    std::unique_ptr<ld::SimInjector> injector;
    ld::DeckSink* sink = nullptr;
    if (simulate) {
      sim_sink = std::make_unique<ld::SimulatorSink>(dir / "sim");
      sink = sim_sink.get();
    } else {
      // The vendor app that takes the device when it runs (none = never released for another app).
      std::function<bool()> vendor_running;
      if (!active.layout.vendor_app.empty())
        vendor_running = [exe = ld::widen(active.layout.vendor_app)] { return ld::process_running(exe); };
      device = std::make_unique<ld::DeviceManager>(
          loop, active, [&active] { return ld::open_hid(active.layout.usb); }, std::move(vendor_running));
      sink = device.get();
    }

    ld::WinHttpClient http(loop);
    ld::StateStore state(dir / "state.json");
    ld::Deck deck(loop, renderer, *sink, active.layout, ld::DeckServices{&http, &state});
    deck.apply(initial);

    ld::PowerState power;
    auto go_sleep = [&] {
      deck.suspend();
      if (device) device->sleep();
    };
    auto go_wake = [&] {
      deck.resume();
      if (device) device->wake();
    };

    std::unique_ptr<ld::WebUi> web;  // declared before the window: the tray callback may use it
    ld::AppWindow window(instance, ld::AppWindow::Callbacks{
        .on_quit = [&] {
          watchdog->arm();
          if (device) device->shutdown();
          loop.quit();
        },
        .on_lock = [&] { if (power.on_lock()) go_sleep(); },
        .on_unlock = [&] { if (power.on_unlock()) go_wake(); },
        .on_suspend = [&] { if (power.on_suspend()) go_sleep(); },
        .on_resume = [&] { if (power.on_resume()) go_wake(); },
        .on_end_session = [&] {
          if (device) device->shutdown();
        },
        .on_configure = [&] {
          if (web && web->running()) ld::open_url(web->config_url());
        },
        .on_device_arrival = [&] {
          if (device) device->device_arrived();
        },
        .accepts_device = [&active](std::wstring_view path) { return ld::matches_usb_path(path, active.layout.usb); },
    });

    std::wstring config_alert, web_alert;
    auto refresh_alert = [&] {
      std::wstring text = config_alert;
      if (!web_alert.empty()) text += (text.empty() ? L"" : L"; ") + web_alert;
      window.set_alert(text);
    };

    web = std::make_unique<ld::WebUi>(loop, config, deck, [&]() -> std::string {
      return device ? device_state_name(device->state()) : "simulator";
    });
    deck.on_frame = [&](int target) { web->frame(target); };
    config.on_applied = [&](const ld::Config& before, const ld::Config& after) {
      loop.post([&] { deck.apply(config.current()); });  // on its own turn, never inside a request
      if (device && before.brightness != after.brightness)
        device->set_brightness(static_cast<std::uint8_t>(after.brightness));
      if (before.web_port != after.web_port)
        ld::log_info("web port {} -> {}: takes effect at the next start", before.web_port, after.web_port);
    };
    config.on_status = [&] {
      config_alert = config.last_errors().empty() ? L"" : kConfigAlert;
      refresh_alert();
      web->config_changed();
    };
    ld::ConfigWatcher watcher(loop, config_file, [&] { config.reload(); });

    if (!web->start(initial.web_port))
      web_alert = std::format(L"web interface unavailable (port {} in use)", initial.web_port);
    window.set_configure_enabled(web->running());
    if (!config.last_errors().empty()) config_alert = kConfigAlert;  // invalid file at startup
    refresh_alert();

    if (simulate) {
      injector = std::make_unique<ld::SimInjector>(loop, dir / "sim" / "inject.txt", active.layout,
                                                   [&](const ld::DeviceInput& in) { deck.on_input(in); });
      window.set_status(L"Simulator (" + (dir / L"sim").wstring() + L")");
    } else {
      device->set_brightness(static_cast<std::uint8_t>(initial.brightness));
      device->on_input = [&](const ld::DeviceInput& in) { deck.on_input(in); };
      device->on_connected = [&] { deck.redraw_all(); };
      device->on_state = [&](ld::DeviceManager::State s) {
        window.set_status(device_state_text(s, vendor));
        web->device_changed(device_state_name(s));
      };
      device->start();
      window.set_status(device_state_text(device->state(), vendor));
    }

    running = true;
    code = loop.run();
    watchdog->arm();  // a no-op after Quit; covers an exit of the loop for another reason
    web->stop();  // first: no web thread may keep waiting on a loop that no longer runs
    deck.on_frame = nullptr;
    config.on_applied = nullptr;
    config.on_status = nullptr;
    if (device) {
      device->on_state = nullptr;
      device->on_input = nullptr;
      device->on_connected = nullptr;
    }
    injector.reset();
  } catch (const std::exception& e) {
    fatal(e.what());
  } catch (...) {
    fatal("unknown exception");
  }
  ld::log_info("lite-deck stopped (code {})", code);
  CoUninitialize();
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return code;
}
