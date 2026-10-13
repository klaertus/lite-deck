#include "app/single_instance.h"
#include <windows.h>
#include "app/app_window.h"

namespace ld {

bool signal_running_instance(const wchar_t* window_class, Duration wait) {
  const auto deadline = Clock::now() + wait;
  for (;;) {
    if (HWND h = FindWindowW(window_class, nullptr)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(h, &pid);
      AllowSetForegroundWindow(pid);  // let the running instance bring the browser to the front
      return PostMessageW(h, WM_COMMAND, kCmdConfigure, 0) != FALSE;
    }
    if (Clock::now() >= deadline) return false;
    Sleep(100);
  }
}

}  // namespace ld
