#include <winrt/Windows.Media.Control.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include "core/log.h"
#include "core/winrt.h"
#include "modules/now_playing/now_playing.h"

namespace ld {
namespace {
using namespace winrt::Windows::Media::Control;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

// Shared between the source and every WinRT callback (thread-pool threads). Callbacks hold a
// weak_ptr, lock `mu`, and give up once `stopped` is set, so destroying the source while an
// operation is in flight is safe. `cb` only posts to the loop.
struct State {
  std::recursive_mutex mu;  // recursive: Completed handlers may run inline under the lock
  bool stopped = false;
  std::uint64_t generation = 0;  // bumped on every session change / new read; stale reads are dropped
  std::function<void(std::optional<Track>)> cb;
  std::optional<Track> last;  // last Track reported for the current session (timeline events reuse its text)
  GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
  GlobalSystemMediaTransportControlsSession session{nullptr};
  winrt::event_token session_changed{}, props_changed{}, playback_changed{}, timeline_changed{};
};

void report_locked(State& st, std::optional<Track> t) {
  st.last = t;
  if (!st.stopped && st.cb) st.cb(std::move(t));
}

void detach_session_locked(State& st) {
  if (!st.session) return;
  try {
    st.session.MediaPropertiesChanged(st.props_changed);
    st.session.PlaybackInfoChanged(st.playback_changed);
    st.session.TimelinePropertiesChanged(st.timeline_changed);
  } catch (...) {
  }
  st.session = nullptr;
}

// Position now (seconds) and duration; {0, 0} when the player publishes no timeline.
std::pair<double, double> read_timeline(const GlobalSystemMediaTransportControlsSession& session, bool playing) {
  try {
    auto tl = session.GetTimelineProperties();
    double duration = std::chrono::duration<double>(tl.EndTime() - tl.StartTime()).count();
    if (duration <= 0) return {0, 0};
    double pos = std::chrono::duration<double>(tl.Position() - tl.StartTime()).count();
    if (playing && tl.LastUpdatedTime().time_since_epoch().count() != 0) {
      // Only trust LastUpdatedTime when it is plausible: some players leave it at 0 or never refresh it.
      double elapsed = std::chrono::duration<double>(winrt::clock::now() - tl.LastUpdatedTime()).count();
      if (elapsed >= 0 && elapsed <= duration) pos += elapsed;
    }
    return {std::clamp(pos, 0.0, duration), duration};
  } catch (...) {
    return {0, 0};
  }
}

void refresh_locked(const std::shared_ptr<State>& st) {
  const std::uint64_t gen = ++st->generation;
  if (!st->session) return report_locked(*st, std::nullopt);
  try {
    bool playing = st->session.GetPlaybackInfo().PlaybackStatus() ==
                   GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    auto [pos, duration] = read_timeline(st->session, playing);
    std::weak_ptr<State> weak = st;
    st->session.TryGetMediaPropertiesAsync().Completed([weak, playing, pos, duration, gen](auto const& op, AsyncStatus status) {
      auto s = weak.lock();
      if (!s) return;
      std::lock_guard lock(s->mu);
      if (s->generation != gen) return;
      try {
        if (status == AsyncStatus::Completed) {
          auto props = op.GetResults();
          return report_locked(*s, Track{to_wide(props.Title()), to_wide(props.Artist()), playing, pos, duration});
        }
      } catch (...) {
      }
      // Failed read: keep the last track of this session, but never leave the previous session's on screen.
      if (!s->last) report_locked(*s, std::nullopt);
    });
  } catch (...) {
    report_locked(*st, std::nullopt);
  }
}

// Timeline-only update: keeps the last title and artist, re-reads position and duration, and does not
// bump `generation` (so it cannot cancel a metadata read in flight).
void refresh_timeline_locked(State& st) {
  if (!st.session || !st.last) return;
  try {
    Track t = *st.last;
    t.playing = st.session.GetPlaybackInfo().PlaybackStatus() ==
                GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    std::tie(t.position_s, t.duration_s) = read_timeline(st.session, t.playing);
    report_locked(st, std::move(t));
  } catch (...) {
  }
}

void attach_session_locked(const std::shared_ptr<State>& st) {
  detach_session_locked(*st);
  ++st->generation;
  st->last.reset();
  try {
    st->session = st->manager.GetCurrentSession();
    if (!st->session) return report_locked(*st, std::nullopt);
    std::weak_ptr<State> weak = st;
    auto on_change = [weak](auto&&, auto&&) {
      auto s = weak.lock();
      if (!s) return;
      std::lock_guard lock(s->mu);
      if (!s->stopped) refresh_locked(s);
    };
    st->props_changed = st->session.MediaPropertiesChanged(on_change);
    st->playback_changed = st->session.PlaybackInfoChanged(on_change);
    st->timeline_changed = st->session.TimelinePropertiesChanged([weak](auto&&, auto&&) {
      auto s = weak.lock();
      if (!s) return;
      std::lock_guard lock(s->mu);
      if (!s->stopped) refresh_timeline_locked(*s);
    });
  } catch (...) {
    return report_locked(*st, std::nullopt);
  }
  refresh_locked(st);
}

class GsmtcSource final : public NowPlayingSource {
public:
  // Takes the session and manager out under the lock, then revokes the events (calls into the media
  // service) without holding it: a handler waiting for the lock never blocks the revocation.
  ~GsmtcSource() override {
    State moved;
    {
      std::lock_guard lock(st_->mu);
      st_->stopped = true;
      st_->cb = nullptr;
      moved.session = std::exchange(st_->session, nullptr);
      moved.manager = std::exchange(st_->manager, nullptr);
      moved.props_changed = st_->props_changed;
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
  void start(std::function<void(std::optional<Track>)> on_change) override {
    {
      std::lock_guard lock(st_->mu);
      st_->cb = std::move(on_change);
    }
    std::weak_ptr<State> weak = st_;
    try {
      GlobalSystemMediaTransportControlsSessionManager::RequestAsync().Completed(
          [weak](IAsyncOperation<GlobalSystemMediaTransportControlsSessionManager> const& op, AsyncStatus status) {
            auto s = weak.lock();
            if (!s) return;
            std::lock_guard lock(s->mu);
            if (s->stopped) return;
            if (status != AsyncStatus::Completed) return report_locked(*s, std::nullopt);
            try {
              s->manager = op.GetResults();
              s->session_changed = s->manager.CurrentSessionChanged([weak](auto&&, auto&&) {
                auto s2 = weak.lock();
                if (!s2) return;
                std::lock_guard lock2(s2->mu);
                if (!s2->stopped) attach_session_locked(s2);
              });
            } catch (...) {
              log_warn("now_playing: media session manager unavailable");
              return report_locked(*s, std::nullopt);
            }
            attach_session_locked(s);
          });
    } catch (...) {
      log_warn("now_playing: RequestAsync failed");
      std::lock_guard lock(st_->mu);
      report_locked(*st_, std::nullopt);
    }
  }

private:
  std::shared_ptr<State> st_ = std::make_shared<State>();
};

}  // namespace

std::unique_ptr<NowPlayingSource> make_gsmtc_source() { return std::make_unique<GsmtcSource>(); }

}  // namespace ld
