// src/app/shell.cpp
#include "app/shell.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <thread>
#include "core/log.h"

namespace ld {

bool open_url(const std::wstring& url) {
  if (url.empty()) return false;
  std::thread([url] {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) log_error("cannot open the browser (ShellExecute returned {})", static_cast<long long>(r));
    if (SUCCEEDED(co)) CoUninitialize();
  }).detach();
  return true;
}

}  // namespace ld
