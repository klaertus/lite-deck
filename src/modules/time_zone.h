// src/modules/time_zone.h: picks up a Windows time-zone change while the daemon runs.
#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace ld {

// The UCRT reads the system time zone once per process (localtime_s, mktime). After the user, or
// "set the time zone automatically" on a trip, changes it, the clock, date, sun, weather and stopwatch
// keys would keep the old zone until a restart. Call this before converting to local time: at most
// once per kTimeZoneCheckInterval it compares the system zone with the last one seen and calls
// _tzset() when it changed. True when it reloaded the zone. Thread-safe.
bool refresh_time_zone();

inline constexpr std::chrono::seconds kTimeZoneCheckInterval{60};

// The logic of refresh_time_zone, with the zone source and the reload injected (tests).
class TimeZoneWatch {
public:
  TimeZoneWatch(std::function<std::wstring()> zone_id, std::function<void()> reload)
      : zone_id_(std::move(zone_id)), reload_(std::move(reload)) {}
  // The first check only records the zone; later ones reload when it differs.
  bool check(std::chrono::steady_clock::time_point now);

private:
  std::function<std::wstring()> zone_id_;
  std::function<void()> reload_;
  std::optional<std::chrono::steady_clock::time_point> last_check_;
  std::wstring last_id_;
};

}  // namespace ld
