#include "app/app_window.h"
#include <dbt.h>
#include <hidsdi.h>
#include <wtsapi32.h>
#include <format>
#include <stdexcept>
#include "core/log.h"

#pragma comment(lib, "wtsapi32.lib")

namespace ld {
namespace {
constexpr UINT kMenuChoiceMsg = WM_APP + 2;  // wParam = command chosen in the tray menu (menu thread -> window)
constexpr wchar_t kMenuOwnerClass[] = L"LiteDeckMenuOwner";

// Owner of the tray menu, on the menu thread. WM_CANCELMODE (sent by ~AppWindow) closes the menu.
LRESULT CALLBACK menu_owner_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CANCELMODE) EndMenu();
  return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

AppWindow::AppWindow(HINSTANCE inst, Callbacks cb, const wchar_t* class_name) : cb_(std::move(cb)) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DBLCLKS;  // the tray reports double clicks, which must not open two tabs
  wc.lpfnWndProc = &AppWindow::proc;
  wc.hInstance = inst;
  wc.lpszClassName = class_name;
  if (!RegisterClassExW(&wc)) {
    const DWORD err = GetLastError();
    if (err != ERROR_CLASS_ALREADY_EXISTS)
      throw std::runtime_error(std::format("cannot register the hidden window class ({})", err));
  }
  hwnd_ = CreateWindowExW(0, class_name, L"lite-deck", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, this);
  if (!hwnd_) {
    const DWORD err = GetLastError();
    throw std::runtime_error(std::format("cannot create the hidden window ({})", err));
  }
  taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
  wts_registered_ = WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;
  if (!wts_registered_) {
    const DWORD err = GetLastError();
    log_warn("session notifications unavailable ({}): the deck will not sleep on lock", err);
  }
  add_icon();
  DEV_BROADCAST_DEVICEINTERFACE_W filter{};
  filter.dbcc_size = sizeof(filter);
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  HidD_GetHidGuid(&filter.dbcc_classguid);
  dev_notify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
  if (!dev_notify_) log_warn("device notifications unavailable ({}): falling back to polling", GetLastError());
}

AppWindow::~AppWindow() {
  close_menu();
  if (dev_notify_) UnregisterDeviceNotification(dev_notify_);
  Shell_NotifyIconW(NIM_DELETE, &nid_);
  if (wts_registered_) WTSUnRegisterSessionNotification(hwnd_);
  DestroyWindow(hwnd_);
}

void AppWindow::invoke(const std::function<void()>& fn, const char* what) {
  if (!fn) return;
  try {
    fn();
  } catch (const std::exception& e) {
    log_error("{} threw: {}", what, e.what());
  } catch (...) {
    log_error("{} threw a non-standard exception", what);
  }
}

void AppWindow::fill_icon() {
  nid_.hIcon = LoadIconW(nullptr, alert_.empty() ? IDI_APPLICATION : IDI_WARNING);
  const std::wstring tip = L"lite-deck - " + (alert_.empty() ? status_ : alert_);
  wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
}

void AppWindow::add_icon() {
  nid_ = NOTIFYICONDATAW{sizeof(nid_)};
  nid_.hWnd = hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid_.uCallbackMessage = kTrayMessage;
  fill_icon();
  if (Shell_NotifyIconW(NIM_ADD, &nid_)) {
    icon_failed_ = false;
  } else if (!icon_failed_) {  // e.g. the taskbar is not up yet: TaskbarCreated adds it later
    const DWORD err = GetLastError();
    icon_failed_ = true;
    log_warn("cannot add the tray icon ({}); retrying when the taskbar restarts", err);
  }
}

void AppWindow::refresh_icon() {
  fill_icon();
  nid_.uFlags = NIF_ICON | NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void AppWindow::set_status(const std::wstring& text) {
  status_ = text;
  refresh_icon();
}

void AppWindow::set_alert(const std::wstring& text) {
  if (text == alert_) return;
  alert_ = text;
  refresh_icon();
}

void AppWindow::set_configure_enabled(bool enabled) { configure_enabled_ = enabled; }

void AppWindow::configure() {
  if (configure_enabled_) invoke(cb_.on_configure, "configure");
}

void AppWindow::show_menu() {
  if (menu_open_) return;  // already open: its own thread handles the clicks
  if (menu_thread_.joinable()) menu_thread_.join();  // the previous menu, already finished
  menu_open_ = true;
  menu_thread_ = std::thread(&AppWindow::run_menu, this, status_, alert_, configure_enabled_);
}

// Menu thread. Only reads the snapshot it was given, tracker_ (set before any menu) and atomics.
void AppWindow::run_menu(std::wstring status, std::wstring alert, bool configure_enabled) {
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = &menu_owner_proc;
  wc.hInstance = inst;
  wc.lpszClassName = kMenuOwnerClass;
  RegisterClassExW(&wc);  // fails harmlessly once registered
  HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, kMenuOwnerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                               nullptr);
  if (!owner) {
    const DWORD err = GetLastError();
    log_warn("cannot show the tray menu ({})", err);
    menu_open_ = false;
    return;
  }
  menu_owner_ = owner;
  UINT cmd = 0;
  if (!closing_) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    if (!alert.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"⚠ " + alert).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (configure_enabled ? MF_ENABLED : MF_GRAYED), kCmdConfigure, L"Configure…");
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit");
    if (configure_enabled) SetMenuDefaultItem(menu, kCmdConfigure, FALSE);
    if (tracker_) {
      cmd = tracker_(menu, owner);
    } else {
      POINT pt;
      GetCursorPos(&pt);
      SetForegroundWindow(owner);  // documented TrackPopupMenu requirement: the menu then closes on an outside click
      cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr));
    }
    DestroyMenu(menu);
  }
  menu_owner_ = nullptr;
  DestroyWindow(owner);
  if (cmd && !closing_) PostMessageW(hwnd_, kMenuChoiceMsg, cmd, 0);  // runs on the loop thread
  menu_open_ = false;
}

void AppWindow::close_menu() {
  closing_ = true;
  while (menu_open_) {
    if (HWND owner = menu_owner_) SendMessageTimeoutW(owner, WM_CANCELMODE, 0, 0, SMTO_ABORTIFHUNG, 100, nullptr);
    Sleep(10);
  }
  if (menu_thread_.joinable()) menu_thread_.join();
}

LRESULT CALLBACK AppWindow::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (self && self->hwnd_ == h) return self->handle(msg, wp, lp);
  return DefWindowProcW(h, msg, wp, lp);
}

LRESULT AppWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == kTrayMessage) {
    if (LOWORD(lp) == WM_LBUTTONDBLCLK) {  // a double click sends a second WM_LBUTTONUP: open one tab only
      skip_next_up_ = true;
      return 0;
    }
    if (LOWORD(lp) == WM_LBUTTONUP && skip_next_up_) {
      skip_next_up_ = false;
      return 0;
    }
    if (LOWORD(lp) == WM_LBUTTONUP && configure_enabled_) {
      configure();  // left click: straight to the page
      return 0;
    }
    if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
      show_menu();
      return 0;
    }
  }
  if (msg == kMenuChoiceMsg) {
    if (wp == kCmdQuit) invoke(cb_.on_quit, "quit");
    if (wp == kCmdConfigure) configure();
    return 0;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == kCmdConfigure) {
    configure();
    return 0;
  }
  if (msg == WM_DEVICECHANGE) {
    if (wp == DBT_DEVICEARRIVAL && lp && cb_.on_device_arrival && cb_.accepts_device) {
      const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
      if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
          cb_.accepts_device(reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lp)->dbcc_name))
        invoke(cb_.on_device_arrival, "device arrival");
    }
    return TRUE;
  }
  if (msg == WM_CLOSE) {  // e.g. uninstall script, taskkill without /F: same clean shutdown as "Quit"
    invoke(cb_.on_quit, "quit");
    return 0;
  }
  if (msg == taskbar_created_) {
    add_icon();
    return 0;
  }
  if (msg == WM_WTSSESSION_CHANGE) {
    if (wp == WTS_SESSION_LOCK) invoke(cb_.on_lock, "session lock");
    if (wp == WTS_SESSION_UNLOCK) invoke(cb_.on_unlock, "session unlock");
    return 0;
  }
  if (msg == WM_ENDSESSION) {
    if (wp) invoke(cb_.on_end_session, "end session");
    return 0;
  }
  if (msg == WM_POWERBROADCAST) {
    if (wp == PBT_APMSUSPEND) invoke(cb_.on_suspend, "suspend");
    if (wp == PBT_APMRESUMEAUTOMATIC) invoke(cb_.on_resume, "resume");
    return TRUE;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace ld
