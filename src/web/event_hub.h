// src/web/event_hub.h
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "core/types.h"

namespace ld {

namespace detail {
struct HubCore;
}

// Server-sent events for the config page. The loop thread publishes; each SSE connection (one web
// thread) pulls with Stream::next(). With no page open, publish() and frame() return after a single
// atomic load: the deck pays nothing.
class EventHub {
public:
  static constexpr int kMaxStreams = 2;
  static constexpr Duration kFrameInterval{200};  // previews refresh at most 5 times per second
  static constexpr Duration kPing{5000};          // detects closed pages (frees their thread); keeps the stream alive

  class Stream {
  public:
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    // Blocks until there is something to send. Returns SSE text: events, a ": ping" comment when
    // `ping` elapses with nothing to say, or "" once the hub is closed (end the stream).
    std::string next(Duration ping = kPing);

  private:
    friend class EventHub;
    Stream(std::shared_ptr<detail::HubCore> core, int id) : core_(std::move(core)), id_(id) {}
    std::shared_ptr<detail::HubCore> core_;
    int id_;
  };

  EventHub();
  std::unique_ptr<Stream> subscribe();  // nullptr when kMaxStreams are open or the hub is closed
  void publish(const std::string& event, const nlohmann::json& data);  // latest value per event name
  void frame(int target);  // key 0 .. key_count - 1 or strip (-1) has a new frame; coalesced per stream
  void close();            // ends every stream; later subscribe() calls fail
  // `fn` runs on a web thread each time the last open stream ends (no page open any more). Set it
  // before the server starts.
  void on_idle(std::function<void()> fn);
  int streams() const;

private:
  std::shared_ptr<detail::HubCore> core_;
};

}  // namespace ld
