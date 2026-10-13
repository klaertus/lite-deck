// src/modules/common/temp_header.cpp
#include "modules/common/temp_header.h"
#include <format>
#include "modules/common/format.h"

namespace ld {

std::wstring format_temperature(std::optional<int> celsius) {
  if (!celsius) return std::wstring(kDash);
  return std::format(L"{}{}°C", *celsius, kNarrowNbsp);
}

}  // namespace ld
