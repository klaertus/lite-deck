// src/modules/seek/gsmtc_seek.cpp - SeekTarget on the current Windows media session (GSMTC).
#include <winrt/Windows.Media.Control.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include "core/log.h"
#include "core/winrt.h"
#include "modules/seek/seek.h"

namespace ld {
namespace {
using namespace winrt::Windows::Media::Control;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

// Shared with every WinRT callback (thread-pool threads), which hold a weak_ptr and give up once
// `stopped` is set. Recursive: Completed handlers may run inline under the lock.
struct State {
  std::recursive_mutex mu;
  bool stopped = false;
  std::function<void(bool)> cb;
  GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
  GlobalSystemMediaTransportControlsSession session{nullptr};
  winrt::event_token session_changed{}, playback_changed{}, timeline_changed{};
  // Cache refreshed from the session events, so timeline() never calls the player from the loop thread.
  bool playing = false;
  bool has_timeline = false;
  winrt::Windows::Foundation::TimeSpan start{}, end{}, position{};
  winrt::Windows::Foundation::DateTime updated{};
};

bool is_playing(const GlobalSystemMediaTransportControlsSession& s) {
  return s.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
}

void read_playing_locked(State& st) {
  try {
    st.playing = st.session && is_playing(st.session);
  } catch (...) {
    st.playing = false;
  }
}

void read_timeline_locked(State& st) {
  st.has_timeline = false;
  if (!st.session) return;
  try {
    auto tl = st.session.GetTimelineProperties();
    st.start = tl.StartTime();
    st.end = tl.EndTime();
    st.position = tl.Position();
    st.updated = tl.LastUpdatedTime();
    st.has_timeline = true;
  } catch (...) {
  }
}

void report_locked(State& st) {
  if (st.stopped || !st.cb) return;
  st.cb(st.playing);
}

void detach_session_locked(State& st) {
  if (!st.session) return;
  try {
    st.session.PlaybackInfoChanged(st.playback_changed);
    st.session.TimelinePropertiesChanged(st.timeline_changed);
  } catch (...) {
  }
  st.session = nullptr;
  st.playing = false;
  st.has_timeline = false;
}

void attach_session_locked(const std::shared_ptr<State>& st) {
  detach_session_locked(*st);
  try {
    st->session = st->manager.GetCurrentSession();
    if (st->session) {
      std::weak_ptr<State> weak = st;
      st->playback_changed = st->session.PlaybackInfoChanged([weak](auto&&, auto&&) {
        auto s = weak.lock();
        if (!s) return;
        std::lock_guard lock(s->mu);
        if (s->stopped) return;
        read_playing_locked(*s);
        read_timeline_locked(*s);  // the position jumps on play/pause in some players
        report_locked(*s);
      });
      st->timeline_changed = st->session.TimelinePropertiesChanged([weak](auto&&, auto&&) {
        auto s = weak.lock();
        if (!s) return;
        std::lock_guard lock(s->mu);
        if (!s->stopped) read_timeline_locked(*s);
      });
    }
  } catch (...) {
    st->session = nullptr;
  }
  read_playing_locked(*st);
  read_timeline_locked(*st);
  report_locked(*st);
}

double seconds(winrt::Windows::Foundation::TimeSpan t) { return std::chrono::duration<double>(t).count(); }

class GsmtcSeekTarget final : public SeekTarget {
public:
  // Takes the session and manager out under the lock, then revokes the events (calls into the media
  // service) without holding it, so the loop thread never waits on a handler stuck in the player.
  ~GsmtcSeekTarget() override {
    State moved;
    {
      std::lock_guard lock(st_->mu);
      st_->stopped = true;
      st_->cb = nullptr;
      moved.session = std::exchange(st_->session, nullptr);
      moved.manager = std::exchange(st_->manager, nullptr);
      moved.playback_changed = st_->playback_changed;
      moved.timeline_changed = st_->timeline_changed;
      moved.session_changed = st_->session_changed;
    }
    detach_session_locked(moved);  // `moved` is local: no lock needed
    try {
      if (moved.manager) moved.manager.CurrentSessionChanged(moved.session_changed);
    } catch (...) {
    }
  }

  void start(std::function<void(bool)> on_playing) override {
    {
      std::lock_guard lock(st_->mu);
      st_->cb = std::move(on_playing);
    }
    std::weak_ptr<State> weak = st_;
    try {
      GlobalSystemMediaTransportControlsSessionManager::RequestAsync().Completed(
          [weak](IAsyncOperation<GlobalSystemMediaTransportControlsSessionManager> const& op, AsyncStatus status) {
            auto s = weak.lock();
            if (!s) return;
            std::lock_guard lock(s->mu);
            if (s->stopped) return;
            if (status != AsyncStatus::Completed) return report_locked(*s);
            try {
              s->manager = op.GetResults();
              s->session_changed = s->manager.CurrentSessionChanged([weak](auto&&, auto&&) {
                auto s2 = weak.lock();
                if (!s2) return;
                std::lock_guard lock2(s2->mu);
                if (!s2->stopped) attach_session_locked(s2);
              });
            } catch (...) {
              log_warn("seek: media session manager unavailable");
              return report_locked(*s);
            }
            attach_session_locked(s);
          });
    } catch (...) {
      log_warn("seek: RequestAsync failed");
      std::lock_guard lock(st_->mu);
      report_locked(*st_);
    }
  }

  std::optional<SeekTimeline> timeline() override {
    std::lock_guard lock(st_->mu);
    if (!st_->session) return std::nullopt;
    if (!st_->has_timeline) return SeekTimeline{};  // no timeline: the module ignores the seek
    SeekTimeline out{seconds(st_->start), seconds(st_->end), seconds(st_->position)};
    const double duration = out.end_s - out.start_s;
    if (st_->updated.time_since_epoch().count() != 0) {
      // Same rule as now_playing: only trust a plausible LastUpdatedTime.
      double elapsed = std::chrono::duration<double>(winrt::clock::now() - st_->updated).count();
      if (elapsed >= 0) out.age_s = elapsed;
      if (duration > 0 && st_->playing && elapsed >= 0 && elapsed <= duration) out.position_s += elapsed;
    }
    return out;
  }

  void seek(double position_s, std::function<void(bool)> done) override {
    GlobalSystemMediaTransportControlsSession session{nullptr};
    {
      std::lock_guard lock(st_->mu);
      session = st_->session;
    }
    if (!session) return done(false);
    try {  // outside the lock: an event handler busy with the player does not hold this up
      const auto ticks = static_cast<std::int64_t>(std::llround(position_s * 1e7));  // TimeSpan: 100 ns units
      session.TryChangePlaybackPositionAsync(ticks).Completed(
          [done](IAsyncOperation<bool> const& op, AsyncStatus status) {
            bool ok = false;
            try {
              ok = status == AsyncStatus::Completed && op.GetResults();
            } catch (...) {
            }
            done(ok);
          });
    } catch (...) {
      done(false);
    }
  }

private:
  std::shared_ptr<State> st_ = std::make_shared<State>();
};

}  // namespace

std::unique_ptr<SeekTarget> make_gsmtc_seek_target() { return std::make_unique<GsmtcSeekTarget>(); }

}  // namespace ld
