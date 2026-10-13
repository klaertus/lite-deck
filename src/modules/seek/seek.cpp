// src/modules/seek/seek.cpp
#include "modules/seek/seek.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include "core/log.h"
#include "modules/common/ring.h"
#include "modules/common/state_settings.h"

namespace ld {

void SeekAccumulator::add(int detents, TimePoint now) {
  if (!first_) first_ = now;
  pending_ += detents;
}

std::optional<TimePoint> SeekAccumulator::due() const {
  if (!first_) return std::nullopt;
  return *first_ + window_;
}

int SeekAccumulator::take(TimePoint now) {
  if (!first_ || now < *first_ + window_) return 0;
  const int n = pending_;
  pending_ = 0;
  first_.reset();
  return n;
}

std::optional<double> seek_position(const SeekTimeline& tl, double delta_s) {
  if (!(tl.end_s > tl.start_s)) return std::nullopt;
  return std::clamp(tl.position_s + delta_s, tl.start_s, std::max(tl.start_s, tl.end_s - 1.0));
}

namespace {

constexpr Rgb kDefaultColor{0x4c, 0xd9, 0x64};
constexpr Rgb kDefaultIdle{0x3d, 0x1f, 0x00};
constexpr RingStateSpec kPlaying{"playing", "Playing", "color", "brightness"};
constexpr RingStateSpec kStopped{"stopped", "Stopped", "idle_color", "idle_brightness"};
constexpr int kPlayingBrightness = 30;  // the green LED at 100 % reads white-green
// How long the last commanded target is trusted over a timeline the player has not refreshed since,
// and how long an unanswered seek blocks the next one.
constexpr Duration kTrustCommanded{2000};

enum class Outcome { Done, NoSession, NoTimeline, Refused };

template <class D> double secs(D d) { return std::chrono::duration<double>(d).count(); }

class SeekModule final : public Module {
public:
  explicit SeekModule(std::function<std::unique_ptr<SeekTarget>()> f) : factory_(std::move(f)) {}
  ~SeekModule() override {
    if (timer_ && ctx_) ctx_->cancel(timer_);
  }
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    ctx_ = &ctx;
    step_s_ = std::clamp(s.value("step_s", 10), 1, 60);
    play_ring_ = ring_color(s, kPlaying, kDefaultColor, kPlayingBrightness);
    idle_ring_ = ring_color(s, kStopped, kDefaultIdle, 100);
    ctx.set_ring_color(idle_ring_);
    target_ = factory_ ? factory_() : nullptr;
    if (!target_) return;
    Poster p = ctx.poster();
    std::weak_ptr<bool> alive = alive_;
    target_->start([p, alive, this](bool playing) {
      p.post([alive, this, playing] {
        if (alive.lock()) set_playing(playing);
      });
    });
  }
  void on_event(const InputEvent& e) override {
    if (e.kind != EventKind::Rotate || e.delta == 0) return;  // press: nothing
    acc_.add(e.delta, ctx_->now());
    arm();
  }
  void render(Canvas&) override {}

private:
  void set_playing(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    ctx_->set_ring_color(playing ? play_ring_ : idle_ring_);
  }
  bool in_flight() const { return in_flight_ && ctx_->now() - last_sent_ < kTrustCommanded; }
  void flush() {
    timer_ = 0;
    held_ += acc_.take(ctx_->now());
    if (!in_flight()) send();  // otherwise sent when the answer arrives (or when it is given up)
    arm();
  }
  // One timer: the next accumulator flush, or giving up on an unanswered seek while detents are held.
  void arm() {
    std::optional<TimePoint> when = acc_.due();
    if (held_ != 0 && in_flight_) {
      const TimePoint give_up = last_sent_ + kTrustCommanded;
      if (!when || give_up < *when) when = give_up;
    }
    if (!when || (timer_ && timer_when_ <= *when)) return;
    if (timer_) ctx_->cancel(timer_);
    timer_when_ = *when;
    timer_ = ctx_->at(*when, [this] { flush(); });
  }
  // Players report the new position late: right after a seek, base the next one on the commanded target
  // unless the timeline was refreshed after that seek was sent.
  double base(const SeekTimeline& tl) const {
    if (!last_target_s_) return tl.position_s;
    const auto since = ctx_->now() - last_sent_;
    if (since >= kTrustCommanded) return tl.position_s;
    if (tl.age_s && *tl.age_s < secs(since)) return tl.position_s;
    return *last_target_s_ + (playing_.value_or(false) ? secs(since) : 0.0);
  }
  void send() {
    const int detents = std::exchange(held_, 0);
    if (detents == 0 || !target_) return;
    std::optional<SeekTimeline> tl;
    try {
      tl = target_->timeline();
    } catch (...) {
    }
    if (!tl) return note(Outcome::NoSession);
    SeekTimeline from = *tl;
    from.position_s = base(*tl);
    auto pos = seek_position(from, static_cast<double>(detents) * step_s_);
    if (!pos) return note(Outcome::NoTimeline);
    in_flight_ = true;
    last_target_s_ = *pos;
    last_sent_ = ctx_->now();
    const std::uint64_t seq = ++seq_;
    Poster p = ctx_->poster();
    std::weak_ptr<bool> alive = alive_;
    try {
      target_->seek(*pos, [p, alive, this, seq](bool ok) {
        p.post([alive, this, seq, ok] {
          if (alive.lock()) answered(seq, ok);
        });
      });
    } catch (...) {
      answered(seq, false);
    }
  }
  void answered(std::uint64_t seq, bool ok) {
    if (seq != seq_) return;  // a newer seek was sent after this one timed out
    in_flight_ = false;
    if (!ok) last_target_s_.reset();
    note(ok ? Outcome::Done : Outcome::Refused);
    if (held_ != 0) send();  // detents turned while waiting
  }
  // Logs once per change of outcome, never per detent.
  void note(Outcome o) {
    if (o == last_) return;
    last_ = o;
    switch (o) {
      case Outcome::Done: log_info("seek: seeking works again"); break;
      case Outcome::NoSession: log_info("seek: no media session, ignored"); break;
      case Outcome::NoTimeline: log_info("seek: the player publishes no timeline, ignored"); break;
      case Outcome::Refused: log_info("seek: the player refused the new position"); break;
    }
  }

  std::function<std::unique_ptr<SeekTarget>()> factory_;
  std::unique_ptr<SeekTarget> target_;
  ModuleContext* ctx_ = nullptr;
  // Answers posted after destruction are dropped. In the deck the Poster already drops them once the slot
  // is detached; this also covers a module destroyed while its context lives on (as the tests do), since a
  // seek answer can arrive long after the knob was turned. The destructor's cancel is the same belt and
  // braces for the flush timer.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  SeekAccumulator acc_;
  TimerId timer_ = 0;
  TimePoint timer_when_{};
  int held_ = 0;  // detents flushed but not sent yet (a seek was in flight)
  bool in_flight_ = false;
  std::uint64_t seq_ = 0;
  std::optional<double> last_target_s_;
  TimePoint last_sent_{};
  int step_s_ = 10;
  Rgb play_ring_ = scaled(kDefaultColor, kPlayingBrightness);  // ring while a media plays
  Rgb idle_ring_ = kDefaultIdle;   // ring otherwise
  std::optional<bool> playing_;
  Outcome last_ = Outcome::Done;
};

}  // namespace

std::unique_ptr<Module> make_seek_module(std::function<std::unique_ptr<SeekTarget>()> factory) {
  return std::make_unique<SeekModule>(std::move(factory));
}

ModuleInfo seek_module_info() {
  ModuleInfo m;
  m.id = "seek";
  m.name = "Seek";
  m.description = "Turn: skip forward or back in the current track.";
  m.category = "Media & sound";
  m.hue = "#4cd964";
  m.slots = {SlotKind::Knob};
  nlohmann::json schema = nlohmann::json::parse(R"J({"fields":[
    {"key":"step_s","type":"int","default":10,"min":1,"max":60,"label":"Seconds per step"}]})J");
  schema = with_fields(std::move(schema), ring_fields(kPlaying, kDefaultColor, kPlayingBrightness));
  m.settings_schema = with_fields(std::move(schema), ring_fields(kStopped, kDefaultIdle, 100));
  m.ring_states = {kPlaying, kStopped};
  m.elements = {};  // knobs have no display
  m.create = [] { return make_seek_module(make_gsmtc_seek_target); };
  return m;
}

}  // namespace ld
