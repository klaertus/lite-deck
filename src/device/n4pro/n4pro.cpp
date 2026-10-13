#include "device/n4pro/n4pro.h"
#include <algorithm>

namespace ld::n4pro {

bool N4Pro::send(const proto::Report& r) {
  if (failed_) return false;
  if (!t_.write(r)) failed_ = true;
  return !failed_;
}

bool N4Pro::send_all(const std::vector<proto::Report>& rs) {
  for (const auto& r : rs)
    if (!send(r)) return false;
  return true;
}

void N4Pro::initialize(std::uint8_t brightness) {
  send(proto::dis());
  send(proto::brightness(brightness));
  send(proto::clear(0xFF));
  send(proto::bg_clear(3));
  send(proto::stp());
}

void N4Pro::set_brightness(std::uint8_t pct) { send(proto::brightness(pct)); }

void N4Pro::queue_key(int slot, std::vector<std::uint8_t> jpeg) { pending_keys_[slot] = std::move(jpeg); }

void N4Pro::queue_strip(std::vector<std::uint8_t> jpeg) { pending_strip_ = std::move(jpeg); }

void N4Pro::set_rings(const std::array<Rgb, 4>& colors, std::uint8_t brightness) {
  bool all_black = std::all_of(colors.begin(), colors.end(), [](Rgb c) { return c == Rgb{}; });
  if (all_black || brightness == 0) {
    send(proto::ring_brightness(0));  // SETLB with four black rings is ignored by the firmware
    return;
  }
  send(proto::ring_brightness(brightness));
  send(proto::ring_colors(colors));
}

void N4Pro::heartbeat() { send(proto::connect()); }

void N4Pro::sleep() {
  pending_keys_.clear();
  pending_strip_.reset();
  send(proto::han());
  send(proto::ring_brightness(0));
}

void N4Pro::shutdown() {
  send(proto::disconnect());
  send(proto::han());
}

std::optional<TimePoint> N4Pro::next_flush(TimePoint now) const {
  std::optional<TimePoint> best;
  auto consider = [&](bool pending, const std::optional<TimePoint>& last) {
    if (!pending) return;
    TimePoint t = last ? std::max(now, *last + kFrameInterval) : now;
    if (!best || t < *best) best = t;
  };
  consider(pending_strip_.has_value(), last_strip_);
  consider(!pending_keys_.empty(), last_keys_);
  return best;
}

void N4Pro::flush(TimePoint now) {
  if (pending_strip_ && (!last_strip_ || now >= *last_strip_ + kFrameInterval)) {
    send_all(proto::image_bg(*pending_strip_, 0, 0, 800, 112));
    pending_strip_.reset();
    last_strip_ = now;
  }
  if (!pending_keys_.empty() && (!last_keys_ || now >= *last_keys_ + kFrameInterval)) {
    for (const auto& [slot, jpeg] : pending_keys_) send_all(proto::image_key(proto::key_image_index(slot), jpeg));
    send(proto::stp());
    pending_keys_.clear();
    last_keys_ = now;
  }
}

}  // namespace ld::n4pro
