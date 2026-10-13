// src/sensors/publisher.h
#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include "sensors/sensor_block.h"

namespace ld::sensors {

class TempSource {
public:
  virtual ~TempSource() = default;
  virtual std::optional<double> read_celsius() = 0;  // nullopt = the read failed
};

struct OpenResult {
  std::unique_ptr<TempSource> source;  // null on failure
  Status status = Status::Ok;           // why it failed
  std::string detail;                   // HRESULT, path... for the log
};
using SourceOpener = std::function<OpenResult()>;

inline constexpr std::int64_t kRetryPeriodMs = 30'000;
inline constexpr double kMinPlausibleC = -20.0;
inline constexpr double kMaxPlausibleC = 150.0;

// The service's logic with an injected clock and source. Call tick() once a second.
class Publisher {
public:
  Publisher(SensorBlock& block, SourceOpener opener);
  void tick(std::int64_t now_unix_ms);
  Status status() const { return status_; }
  std::function<void(Status, const std::string& detail)> on_status;  // transitions only

private:
  void publish(double temp, std::int64_t now, Status s, const std::string& detail = {});

  SensorBlock& block_;
  SourceOpener opener_;
  std::unique_ptr<TempSource> source_;
  std::int64_t next_open_ms_ = std::numeric_limits<std::int64_t>::min();
  Status status_ = Status::Starting;
};

}  // namespace ld::sensors
