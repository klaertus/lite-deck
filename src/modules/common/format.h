#pragma once
#include <string>

namespace ld {
inline constexpr wchar_t kNarrowNbsp = L' ';
inline constexpr const wchar_t* kDash = L"-";
std::wstring format_decimal(double value, int decimals);  // "12.47"
std::wstring format_grouped(long long value);             // "98 512" with U+202F
}  // namespace ld
