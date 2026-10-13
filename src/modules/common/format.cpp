#include "modules/common/format.h"
#include <format>

namespace ld {

std::wstring format_decimal(double value, int decimals) {
  return std::format(L"{:.{}f}", value, decimals);
}

std::wstring format_grouped(long long value) {
  std::wstring digits = std::to_wstring(value < 0 ? 0ull - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value));
  std::wstring out;
  int count = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (count && count % 3 == 0) out.insert(out.begin(), kNarrowNbsp);
    out.insert(out.begin(), *it);
    ++count;
  }
  if (value < 0) out.insert(out.begin(), L'-');
  return out;
}

}  // namespace ld
