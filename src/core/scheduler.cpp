// src/core/scheduler.cpp
#include "core/scheduler.h"
#include "core/log.h"
#include <algorithm>
#include <exception>
#include <vector>

namespace ld {

TimerId Scheduler::at(TimePoint when, std::function<void()> fn) {
  TimerId id = next_id_++;
  timers_[id] = Timer{when, Duration{0}, std::move(fn)};
  return id;
}

TimerId Scheduler::every(Duration period, std::function<void()> fn, TimePoint now) {
  TimerId id = next_id_++;
  timers_[id] = Timer{now + period, period, std::move(fn)};
  return id;
}

void Scheduler::cancel(TimerId id) { timers_.erase(id); }

std::optional<TimePoint> Scheduler::next_deadline() const {
  std::optional<TimePoint> best;
  for (const auto& [id, t] : timers_)
    if (!best || t.deadline < *best) best = t.deadline;
  return best;
}

void Scheduler::run_due(TimePoint now) {
  std::vector<std::pair<TimePoint, TimerId>> due;
  for (const auto& [id, t] : timers_)
    if (t.deadline <= now) due.emplace_back(t.deadline, id);
  std::sort(due.begin(), due.end());
  for (const auto& [deadline, id] : due) {
    auto it = timers_.find(id);
    if (it == timers_.end() || it->second.deadline > now) continue;
    std::function<void()> fn = it->second.fn;  // copy: the callback may cancel itself
    if (it->second.period.count() > 0) {
      it->second.deadline += it->second.period;
      if (it->second.deadline <= now) it->second.deadline = now + it->second.period;
    } else {
      timers_.erase(it);
    }
    try {
      fn();
    } catch (const std::exception& e) {
      log_error("timer callback threw: {}", e.what());
    } catch (...) {
      log_error("timer callback threw a non-standard exception");
    }
  }
}

}  // namespace ld
