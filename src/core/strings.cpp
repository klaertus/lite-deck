#include "core/strings.h"
#include <windows.h>
#include <climits>

namespace ld {

// Without MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS, Windows replaces invalid sequences with
// U+FFFD instead of failing. A failed call (n <= 0) yields an empty string, never garbage.
std::wstring widen(std::string_view utf8) {
  if (utf8.empty() || utf8.size() > static_cast<std::size_t>(INT_MAX)) return {};
  const int len = static_cast<int>(utf8.size());
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, out.data(), n);
  out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
  return out;
}

std::string narrow(std::wstring_view wide) {
  if (wide.empty() || wide.size() > static_cast<std::size_t>(INT_MAX)) return {};
  const int len = static_cast<int>(wide.size());
  int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, out.data(), n, nullptr, nullptr);
  out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
  return out;
}

}  // namespace ld
