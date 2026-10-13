// src/modules/now_playing/now_playing.h
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "modules/module.h"

namespace ld {
struct Track {
  std::wstring title, artist;
  bool playing = false;
  double position_s = 0;  // playback position when reported (live: + elapsed while playing)
  double duration_s = 0;  // 0 = the player does not report a timeline
  bool operator==(const Track&) const = default;
};
// on_change may run on any thread (WinRT); nullopt = no media session.
class NowPlayingSource {
public:
  virtual ~NowPlayingSource() = default;
  virtual void start(std::function<void(std::optional<Track>)> on_change) = 0;
};
std::unique_ptr<NowPlayingSource> make_gsmtc_source();
std::wstring track_text(const Track& t);
std::unique_ptr<Module> make_now_playing_module(std::function<std::unique_ptr<NowPlayingSource>()> factory);
ModuleInfo now_playing_module_info();
}  // namespace ld
