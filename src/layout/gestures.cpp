#include "layout/gestures.h"
#include <algorithm>
#include <cstdlib>

namespace ld {

std::optional<Gesture> GestureRecognizer::down(int x, TimePoint t) {
  active_ = true;
  long_fired_ = false;
  x0_ = last_x_ = x;
  max_dev_ = 0;
  t0_ = t;
  return std::nullopt;
}

std::optional<Gesture> GestureRecognizer::move(int x, TimePoint t) {
  if (!active_) return down(x, t);
  last_x_ = x;
  max_dev_ = std::max(max_dev_, std::abs(x - x0_));
  if (!long_fired_ && max_dev_ < kMoveTolerance && t - t0_ >= kLongTouch) {
    long_fired_ = true;
    return Gesture{GestureKind::LongTouch, x0_};
  }
  return std::nullopt;
}

std::optional<Gesture> GestureRecognizer::up(TimePoint t) {
  if (!active_) return std::nullopt;
  active_ = false;
  if (long_fired_) return std::nullopt;
  if (max_dev_ < kMoveTolerance && t - t0_ >= kLongTouch) return Gesture{GestureKind::LongTouch, x0_};
  int dx = last_x_ - x0_;
  if (std::abs(dx) >= kSwipeMin) return Gesture{dx > 0 ? GestureKind::SwipeRight : GestureKind::SwipeLeft, x0_};
  if (max_dev_ < kMoveTolerance) return Gesture{GestureKind::Tap, x0_};
  return std::nullopt;
}

}  // namespace ld
