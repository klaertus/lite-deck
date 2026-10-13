// src/app/app_window.h
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace ld {

// Class of the hidden window: a second launch of the daemon finds the running one with
// FindWindowW(kAppWindowClass, nullptr).
inline constexpr wchar_t kAppWindowClass[] = L"LiteDeckHiddenWindow";
// WM_COMMAND id that opens the configuration page: from the tray menu, or posted by another process
// with PostMessageW(FindWindowW(kAppWindowClass, nullptr), WM_COMMAND, kCmdConfigure, 0).
inline constexpr UINT kCmdConfigure = 2;
inline constexpr UINT kCmdQuit = 1;               // tray menu item
inline constexpr UINT kTrayMessage = WM_APP + 1;  // the tray icon's callback message (lParam = mouse message)

// Hidden window: tray icon + menu, session lock and power notifications. Lives on the loop thread,
// so its callbacks run there; an exception thrown by a callback is logged, never propagated.
// The tray menu is modal, so it runs on a short-lived thread of its own: the loop keeps running
// while it is open, and the chosen item comes back to this window as a posted message.
class AppWindow {
public:
  struct Callbacks {
    std::function<void()> on_quit, on_lock, on_unlock, on_suspend, on_resume, on_end_session, on_configure,
        on_device_arrival;
    // Which arrivals (WM_DEVICECHANGE interface path) are the active device's; null: none reaches
    // on_device_arrival.
    std::function<bool(std::wstring_view path)> accepts_device;
  };
  // Shows `menu` owned by `owner` and returns the chosen command (0 = none). Runs on the menu thread.
  using MenuTracker = std::function<UINT(HMENU menu, HWND owner)>;

  // class_name is injectable so tests never collide with a real running instance.
  // Throws std::runtime_error if the window cannot be created.
  AppWindow(HINSTANCE instance, Callbacks callbacks, const wchar_t* class_name = kAppWindowClass);
  ~AppWindow();
  AppWindow(const AppWindow&) = delete;
  AppWindow& operator=(const AppWindow&) = delete;

  void set_status(const std::wstring& text);
  // Non-empty: warning icon, and the text in the tooltip and the menu (invalid config, web UI off).
  void set_alert(const std::wstring& text);
  // "Configure…" (menu, left click, kCmdConfigure) only works while the web UI runs.
  void set_configure_enabled(bool enabled);
  HWND hwnd() const { return hwnd_; }
  // Tests: replaces TrackPopupMenu, so no real menu appears.
  void set_menu_tracker(MenuTracker tracker) { tracker_ = std::move(tracker); }

private:
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void invoke(const std::function<void()>& fn, const char* what);
  void fill_icon();
  void add_icon();
  void refresh_icon();
  void show_menu();
  void run_menu(std::wstring status, std::wstring alert, bool configure_enabled);
  void close_menu();
  void configure();

  HWND hwnd_ = nullptr;
  NOTIFYICONDATAW nid_{};
  Callbacks cb_;
  std::wstring status_ = L"Starting…";
  std::wstring alert_;
  bool configure_enabled_ = false;
  bool skip_next_up_ = false;  // WM_LBUTTONUP that ends a double click
  bool icon_failed_ = false;   // NIM_ADD failure already logged
  UINT taskbar_created_ = 0;
  HDEVNOTIFY dev_notify_ = nullptr;
  bool wts_registered_ = false;
  MenuTracker tracker_;
  std::thread menu_thread_;
  std::atomic<bool> menu_open_{false};
  std::atomic<bool> closing_{false};
  std::atomic<HWND> menu_owner_{nullptr};
};

}  // namespace ld
