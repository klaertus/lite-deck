// src/modules/launcher/launcher.h: a key that starts a program, opens a URL, a file or a folder,
// or runs a command line, always with the user's rights.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "modules/common/action_key.h"
#include "modules/module.h"

namespace ld {

enum class LaunchKind { Program, Open, Command };
enum class Shell { Cmd, PowerShell };

// What to start, environment variables already expanded in the paths.
struct LaunchRequest {
  LaunchKind kind = LaunchKind::Program;
  std::wstring path;         // Program: an exe or a file; Open: a URL, a file or a folder
  std::wstring args;         // Program: passed as they are
  std::wstring working_dir;  // Program: "" = the program's folder
  std::wstring command;      // Command: the command line, run by `shell`
  Shell shell = Shell::Cmd;
  bool hidden = false;       // Command: no console window
  // Program: ShellExecuteExW even for an exe, so Windows applies the exe's manifest (an exe that
  // asks for the highest rights may show the UAC prompt). `path` must be a full path: a bare name
  // is refused (error 2), never searched.
  bool via_shell = false;
  bool operator==(const LaunchRequest&) const = default;
};

// Starts things. Both run on a worker thread and may block.
class Launcher {
public:
  virtual ~Launcher() = default;
  // nullopt when started; else why not, without the arguments or the command line ("error 2: ...").
  virtual std::optional<std::string> launch(const LaunchRequest& request) = 0;
  // Brings to the front the main window of a process running `exe` in this session, restored if
  // minimized. False when there is none.
  virtual bool focus_existing(const std::wstring& exe) = 0;
};
// Whether `request` starts an exe itself, with CreateProcessW: a program or an open target ending
// in .exe. Such a start is never elevated (an exe that requires administrator rights fails with
// error 740). Anything else (a document, a shortcut, an .msi, a .msc, a folder, a URL) is opened by
// ShellExecuteExW: lite-deck never asks for elevation, but Windows may show its UAC prompt when
// the target itself requires it. A command runs its shell through CreateProcessW.
bool runs_directly(const LaunchRequest& request);
// CreateProcessW or ShellExecuteExW ("open") as runs_directly() says; a bare exe name is looked up
// on the PATH only.
std::shared_ptr<Launcher> system_launcher();
// GetSystemDirectoryW ("C:\Windows\System32"), without a trailing backslash.
std::wstring system_directory();

// The `open` action of a typed target (a URL, a file, a folder or a program): one pair of quotes
// removed, %VARIABLES% expanded. The launcher's open kind and the monitor keys' tap action.
LaunchRequest open_request(std::string_view target);
// "tools\app.exe": a folder, but not from a drive or a share. It would resolve against the
// daemon's own current directory, which the user never chose: refused as "Full path?".
bool relative_with_folder(const std::wstring& path);

// The top-level windows, for focus_existing.
struct WindowInfo {
  std::uintptr_t id = 0;   // the HWND
  std::wstring exe;        // the full path of its process's image ("" when unknown)
  bool visible = false;
  bool minimized = false;
  bool owned = false;      // a dialog or a tool window: never the main window
  bool titled = false;
};
class WindowSystem {
public:
  virtual ~WindowSystem() = default;
  virtual std::vector<WindowInfo> windows() = 0;  // in z-order, the front first
  // Restores `id` when asked, then brings it to the front; false when Windows refused.
  virtual bool activate(std::uintptr_t id, bool restore) = 0;
};
// The first visible, titled, unowned window of a process running `exe` (the same full path, or
// the same file name when `exe` has no folder) is activated, restored if minimized. True when
// such a window was found.
bool focus_existing_window(WindowSystem& windows, const std::wstring& exe);

// The command line of `shell` running `command`: cmd /d /s /c "...", or PowerShell with the
// command encoded (-EncodedCommand), so no quoting can change it.
std::wstring shell_command_line(Shell shell, const std::wstring& command);
// The exe and its arguments on one command line: the exe quoted, the arguments as they are.
std::wstring program_command_line(const std::wstring& exe, const std::wstring& args);
// The key's default label: the program's or the file's name without its extension, the URL's
// host (without "www."), the folder's name, or the shell's name.
std::wstring launch_label(const LaunchRequest& request);

// Empty arguments: system_launcher() and the system icons.
std::unique_ptr<Module> make_launcher_module(std::shared_ptr<Launcher> launcher, ActionDeps deps);
ModuleInfo launcher_module_info();

}  // namespace ld
