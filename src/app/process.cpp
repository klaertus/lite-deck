#include "app/process.h"
#include <windows.h>
#include <tlhelp32.h>

namespace ld {

bool process_running(std::wstring_view exe_name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e{};
  e.dwSize = sizeof(e);
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
    found = CompareStringOrdinal(e.szExeFile, -1, exe_name.data(), static_cast<int>(exe_name.size()), TRUE) == CSTR_EQUAL;
  CloseHandle(snap);
  return found;
}

}  // namespace ld
