#pragma once
#include <ctime>
#include <string>
#include "modules/module.h"

namespace ld {
struct DateText {
  std::wstring weekday, day, month;
};
DateText date_text(const std::tm& t);
using MakeTime = std::time_t (*)(std::tm*);
// Real time until the next local midnight (23 h or 25 h on DST days). nullptr = std::mktime.
// The date key wakes up at least this often to notice a clock or time-zone change.
inline constexpr Duration kDateMaxSleep{10 * 60 * 1000};
Duration until_next_midnight(const std::tm& local_now, MakeTime make_time = nullptr);
ModuleInfo date_module_info();
}  // namespace ld
