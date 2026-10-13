#pragma once
#include "core/types.h"

namespace ld {
inline constexpr wchar_t kSingleInstanceMutex[] = L"Local\\LiteDeckSingleInstance";

// Second launch: finds the running instance's hidden window (by class) and makes it open its config page
// (WM_COMMAND kCmdConfigure). Retries for `wait` in case the first instance is still starting.
// False if no window was found.
bool signal_running_instance(const wchar_t* window_class, Duration wait = Duration(5000));
}  // namespace ld
