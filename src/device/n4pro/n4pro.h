#pragma once
#include <array>
#include <map>
#include <optional>
#include <vector>
#include "device/n4pro/protocol.h"
#include "device/transport.h"

namespace ld::n4pro {

// Device-level driver: command sequences, paced image queue, LEDs. No timers of its own:
// the owner calls flush() at next_flush().
class N4Pro {
public:
  static constexpr Duration kFrameInterval{34};  // <= 30 sends per second per channel

  explicit N4Pro(Transport& transport) : t_(transport) {}

  void initialize(std::uint8_t brightness);
  void set_brightness(std::uint8_t pct);
  void queue_key(int slot, std::vector<std::uint8_t> jpeg);
  void queue_strip(std::vector<std::uint8_t> jpeg);
  void set_rings(const std::array<Rgb, 4>& colors, std::uint8_t brightness);
  void heartbeat();
  void sleep();
  void shutdown();

  std::optional<TimePoint> next_flush(TimePoint now) const;
  void flush(TimePoint now);
  bool failed() const { return failed_; }

private:
  bool send(const proto::Report& r);
  bool send_all(const std::vector<proto::Report>& rs);
  Transport& t_;
  bool failed_ = false;
  std::map<int, std::vector<std::uint8_t>> pending_keys_;
  std::optional<std::vector<std::uint8_t>> pending_strip_;
  std::optional<TimePoint> last_keys_, last_strip_;
};

}  // namespace ld::n4pro
