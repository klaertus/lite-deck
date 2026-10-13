// src/modules/time_zone.cpp
#include "modules/time_zone.h"
#include <windows.h>
#include <ctime>
#include <format>
#include <mutex>
#include "core/log.h"
#include "core/strings.h"

namespace ld {

bool TimeZoneWatch::check(std::chrono::steady_clock::time_point now) {
  if (last_check_ && now - *last_check_ < kTimeZoneCheckInterval) return false;
  const bool first = !last_check_;
  last_check_ = now;
  std::wstring id = zone_id_();
  if (first || id == last_id_) {
    last_id_ = std::move(id);
    return false;
  }
  last_id_ = std::move(id);
  reload_();
  return true;
}

namespace {

// The zone and its rules as Windows reports them: a change of any of them is a change of local time.
std::wstring system_zone_id() {
  DYNAMIC_TIME_ZONE_INFORMATION tz{};
  const DWORD r = GetDynamicTimeZoneInformation(&tz);
  return std::format(L"{}|{}|{}|{}|{}|{}", tz.TimeZoneKeyName, tz.Bias, tz.StandardBias, tz.DaylightBias,
                     static_cast<int>(tz.DynamicDaylightTimeDisabled), r);
}

}  // namespace

bool refresh_time_zone() {
  static std::mutex mutex;
  static TimeZoneWatch watch(system_zone_id, [] {
    _tzset();
    log_info("time zone changed to {}, local times follow", narrow(system_zone_id()));
  });
  std::lock_guard lock(mutex);
  return watch.check(std::chrono::steady_clock::now());
}

}  // namespace ld
