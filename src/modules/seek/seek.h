// src/modules/seek/seek.h - knob that moves the current media session forward / back (knob 4 by default).
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include "core/types.h"
#include "modules/module.h"

namespace ld {

// Playback timeline of the current media session, in seconds (same basis as GSMTC: absolute times).
// position_s is "now": already extrapolated while playing.
struct SeekTimeline {
  double start_s = 0;
  double end_s = 0;
  double position_s = 0;
  std::optional<double> age_s;  // seconds since the player last updated it (nullopt: unknown)
};

// Coalesces knob detents: the net count is due `window` after the first detent of a batch, so one seek
// is sent at most every `window` while turning, and `window` after the last detent of a short burst.
class SeekAccumulator {
public:
  explicit SeekAccumulator(Duration window = Duration(150)) : window_(window) {}
  void add(int detents, TimePoint now);
  std::optional<TimePoint> due() const;  // nullopt when nothing is pending
  int take(TimePoint now);               // net detents if due (then resets), else 0

private:
  Duration window_;
  int pending_ = 0;
  std::optional<TimePoint> first_;
};

// Target position: current + delta, clamped to [start, end - 1 s] (a seek onto EndTime skips to the next
// track in some players); nullopt when there is no timeline.
std::optional<double> seek_position(const SeekTimeline& tl, double delta_s);

// The media session the knob acts on.
class SeekTarget {
public:
  virtual ~SeekTarget() = default;
  // on_playing(playing) may run on any thread (WinRT); called on start and whenever the state may change.
  virtual void start(std::function<void(bool playing)> on_playing) = 0;
  virtual std::optional<SeekTimeline> timeline() = 0;  // nullopt = no media session
  // done(accepted) may run on any thread, possibly inline; false = no session or the player refused.
  virtual void seek(double position_s, std::function<void(bool accepted)> done) = 0;
};
std::unique_ptr<SeekTarget> make_gsmtc_seek_target();

std::unique_ptr<Module> make_seek_module(std::function<std::unique_ptr<SeekTarget>()> factory);
ModuleInfo seek_module_info();

}  // namespace ld
