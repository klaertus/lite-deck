#pragma once
#include <optional>
#include "core/types.h"

namespace ld {

enum class GestureKind { Tap, LongTouch, SwipeLeft, SwipeRight };
struct Gesture {
  GestureKind kind;
  int start_x;
};

// Turns the strip's raw touch stream (down / ARX moves / up) into gestures.
class GestureRecognizer {
public:
  static constexpr int kMoveTolerance = 20;
  static constexpr int kSwipeMin = 40;
  static constexpr Duration kLongTouch{500};

  std::optional<Gesture> down(int x, TimePoint t);
  std::optional<Gesture> move(int x, TimePoint t);
  std::optional<Gesture> up(TimePoint t);

private:
  bool active_ = false;
  bool long_fired_ = false;
  int x0_ = 0;
  int last_x_ = 0;
  int max_dev_ = 0;
  TimePoint t0_{};
};

}  // namespace ld
