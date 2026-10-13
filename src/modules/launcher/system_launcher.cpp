// src/modules/launcher/system_launcher.cpp: the Windows side of the launcher. Runs on the module's
// worker thread.
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <format>
#include <map>
#include "core/strings.h"
#include "modules/common/action_key.h"
#include "modules/launcher/launcher.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace ld {
namespace {

// "error 2: The system cannot find the file specified" (never the arguments).
std::string win_error(DWORD e) {
  if (e == ERROR_ELEVATION_REQUIRED) return "error 740: the program requires administrator rights";
  wchar_t* msg = nullptr;
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, e, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::wstring text = n && msg ? std::wstring(msg, n) : std::wstring();
  if (msg) LocalFree(msg);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.'))
    text.pop_back();
  return text.empty() ? std::format("error {}", e) : std::format("error {}: {}", e, narrow(text));
}

void ensure_com() {  // ShellExecuteEx may use COM (shell extensions): an apartment on this worker
  thread_local const bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
  (void)ok;
}

std::wstring folder_of(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace

std::wstring system_directory() {
  wchar_t buf[MAX_PATH] = {};
  const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring(L"C:\\Windows\\System32");
}

namespace {

// CreateProcessW, never elevated (an exe whose manifest requires it fails with error 740). The new
// process may take the foreground.
std::optional<std::string> create_process(const wchar_t* app, std::wstring line, const std::wstring& dir, DWORD flags) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(app, line.data(), nullptr, nullptr, FALSE, flags | CREATE_DEFAULT_ERROR_MODE, nullptr,
                      dir.empty() ? nullptr : dir.c_str(), &si, &pi))
    return win_error(GetLastError());
  AllowSetForegroundWindow(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return std::nullopt;
}

// ShellExecuteExW "open": the default program of a file, a folder or a URL.
std::optional<std::string> shell_open(const std::wstring& file, const std::wstring& params, const std::wstring& dir) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"open";
  info.lpFile = file.c_str();
  info.lpParameters = params.empty() ? nullptr : params.c_str();
  info.lpDirectory = dir.empty() ? nullptr : dir.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) return win_error(GetLastError());
  if (info.hProcess) {
    AllowSetForegroundWindow(GetProcessId(info.hProcess));
    CloseHandle(info.hProcess);
  }
  return std::nullopt;
}

class SystemWindows final : public WindowSystem {
public:
  std::vector<WindowInfo> windows() override {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    struct Walk {
      DWORD session;
      std::map<DWORD, std::wstring> exes;  // per process, looked up once
      std::vector<WindowInfo> out;
    } walk{session, {}, {}};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
          auto& w = *reinterpret_cast<Walk*>(lp);
          DWORD pid = 0;
          GetWindowThreadProcessId(h, &pid);
          DWORD s = 0;
          if (!pid || !ProcessIdToSessionId(pid, &s) || s != w.session) return TRUE;
          BOOL cloaked = FALSE;  // a suspended UWP app's window: present, never shown
          DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
          WindowInfo info;
          info.id = reinterpret_cast<std::uintptr_t>(h);
          info.visible = IsWindowVisible(h) && !cloaked;
          info.minimized = IsIconic(h);
          info.owned = GetWindow(h, GW_OWNER) != nullptr || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
          info.titled = GetWindowTextLengthW(h) > 0;
          auto it = w.exes.find(pid);
          if (it == w.exes.end()) {
            std::wstring exe;
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
              wchar_t buf[MAX_PATH * 2] = {};
              DWORD n = MAX_PATH * 2;
              if (QueryFullProcessImageNameW(p, 0, buf, &n)) exe.assign(buf, n);
              CloseHandle(p);
            }
            it = w.exes.emplace(pid, std::move(exe)).first;
          }
          info.exe = it->second;
          w.out.push_back(std::move(info));
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&walk));
    return std::move(walk.out);
  }

  bool activate(std::uintptr_t id, bool restore) override {
    HWND h = reinterpret_cast<HWND>(id);
    if (IsHungAppWindow(h)) return false;  // "Not responding": never wait on it (found all the same)
    // Asynchronous where Windows offers it: a window whose thread stops answering never blocks this worker.
    if (restore) ShowWindowAsync(h, SW_RESTORE);
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return true;
    // A background process may not take the foreground: share the input state of the foreground
    // window's thread for the call.
    const DWORD me = GetCurrentThreadId();
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = fg && fg != me && AttachThreadInput(me, fg, TRUE);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, fg, FALSE);
    return GetForegroundWindow() == h;
  }
};

class SystemLauncher final : public Launcher {
public:
  std::optional<std::string> launch(const LaunchRequest& r) override {
    ensure_com();
    if (r.kind == LaunchKind::Command) {
      const std::wstring exe = system_directory() + (r.shell == Shell::Cmd ? L"\\cmd.exe" : L"\\WindowsPowerShell\\v1.0\\powershell.exe");
      wchar_t home[MAX_PATH] = {};
      const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
      return create_process(exe.c_str(), shell_command_line(r.shell, r.command),
                            n > 0 && n < MAX_PATH ? std::wstring(home) : std::wstring(),
                            r.hidden ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE);
    }
    if (!runs_directly(r)) {  // a document, a shortcut, a folder, a URL: their default program
      if (r.kind == LaunchKind::Open) return shell_open(r.path, {}, {});
      // A via_shell exe comes with its full path: its manifest decides about elevation, so a bare
      // name is never searched (the daemon's current directory, the PATH).
      if (r.via_shell && folder_of(r.path).empty()) return win_error(ERROR_FILE_NOT_FOUND);
      return shell_open(r.path, r.args, r.working_dir);
    }
    // An exe, whether "program" or "open": started here, never elevated. A bare name is looked up
    // on the PATH only, never in the daemon's current directory.
    std::wstring exe = r.path;
    if (folder_of(exe).empty()) {
      exe = find_on_path(exe);
      if (exe.empty()) return win_error(ERROR_FILE_NOT_FOUND);
    }
    const std::wstring args = r.kind == LaunchKind::Program ? r.args : std::wstring();
    const std::wstring dir = r.kind == LaunchKind::Program && !r.working_dir.empty() ? r.working_dir : folder_of(exe);
    return create_process(exe.c_str(), program_command_line(exe, args), dir, CREATE_NEW_CONSOLE);
  }

  bool focus_existing(const std::wstring& exe) override {
    SystemWindows windows;
    std::wstring path = exe;
    if (path.find_first_of(L"\\/") != std::wstring::npos) {  // "C:\x\..\app.exe" as the process reports it
      wchar_t buf[MAX_PATH * 2] = {};
      const DWORD n = GetFullPathNameW(exe.c_str(), MAX_PATH * 2, buf, nullptr);
      if (n > 0 && n < MAX_PATH * 2) path.assign(buf, n);
    }
    return focus_existing_window(windows, path);
  }
};

}  // namespace

std::shared_ptr<Launcher> system_launcher() { return std::make_shared<SystemLauncher>(); }

}  // namespace ld
