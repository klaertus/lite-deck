#pragma once
#include <string>
#include <string_view>

namespace ld {
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
}  // namespace ld
