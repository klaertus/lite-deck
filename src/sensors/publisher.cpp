// src/sensors/publisher.cpp
#include "sensors/publisher.h"
#include <cmath>
#include <exception>
#include <utility>

namespace ld::sensors {

Publisher::Publisher(SensorBlock& block, SourceOpener opener) : block_(block), opener_(std::move(opener)) {
  write_block(block_, 0.0, 0, Status::Starting);
}

void Publisher::publish(double temp, std::int64_t now, Status s, const std::string& detail) {
  write_block(block_, temp, now, s);
  if (s != status_) {
    status_ = s;
    if (on_status) on_status(s, detail);
  }
}

void Publisher::tick(std::int64_t now) {
  if (!source_) {
    if (now < next_open_ms_ && next_open_ms_ - now > kRetryPeriodMs) next_open_ms_ = now;  // wall clock jumped back
    if (now < next_open_ms_) return;
    OpenResult r;
    try {
      r = opener_();
    } catch (const std::exception& e) {
      r = OpenResult{nullptr, Status::ReadFailed, std::string("open threw: ") + e.what()};
    } catch (...) {
      r = OpenResult{nullptr, Status::ReadFailed, "open threw"};
    }
    if (!r.source) {
      next_open_ms_ = now + kRetryPeriodMs;
      publish(0.0, now, r.status == Status::Ok ? Status::ReadFailed : r.status, r.detail);
      return;
    }
    source_ = std::move(r.source);
  }
  std::optional<double> t;
  bool threw = false;
  try {
    t = source_->read_celsius();
  } catch (...) {
    threw = true;
  }
  if (threw || !t || !(std::isfinite(*t) && *t >= kMinPlausibleC && *t <= kMaxPlausibleC)) {
    source_.reset();
    next_open_ms_ = now + kRetryPeriodMs;
    std::string detail = "read failed";
    if (!threw && t) detail = "implausible value " + std::to_string(*t);
    publish(0.0, now, Status::ReadFailed, detail);
    return;
  }
  publish(*t, now, Status::Ok);
}

}  // namespace ld::sensors
