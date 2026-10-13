// src/core/scheduler.h
#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include "core/types.h"

namespace ld {

// Single-threaded timer set. Small (tens of timers), so linear scans are fine.
class Scheduler {
public:
  TimerId at(TimePoint when, std::function<void()> fn);
  TimerId every(Duration period, std::function<void()> fn, TimePoint now);
  void cancel(TimerId id);
  std::optional<TimePoint> next_deadline() const;
  void run_due(TimePoint now);
  std::size_t size() const { return timers_.size(); }

private:
  struct Timer {
    TimePoint deadline;
    Duration period{0};
    std::function<void()> fn;
  };
  std::map<TimerId, Timer> timers_;
  TimerId next_id_ = 1;
};

}  // namespace ld
