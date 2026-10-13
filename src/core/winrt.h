// src/core/winrt.h - single include point for C++/WinRT (SDK projection headers).
#pragma once
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <string>

namespace ld {
inline std::wstring to_wide(const winrt::hstring& s) { return std::wstring(s.c_str(), s.size()); }
}  // namespace ld
