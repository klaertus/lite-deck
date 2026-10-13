#pragma once
#include <functional>
#include <memory>
#include <optional>

namespace ld {

enum class AudioFlow { Render, Capture };

class AudioEndpoint {
public:
  virtual ~AudioEndpoint() = default;
  virtual std::optional<float> volume() = 0;  // 0..1
  virtual bool set_volume(float v) = 0;
  virtual std::optional<bool> muted() = 0;
  virtual bool set_muted(bool m) = 0;
};

// The default endpoint of one flow, kept up to date. watch() callbacks may run on any thread
// (COM notification threads): they must only post to the loop.
class AudioEndpointProvider {
public:
  virtual ~AudioEndpointProvider() = default;
  virtual AudioEndpoint* endpoint() = 0;  // nullptr when there is no device
  virtual void reopen() = 0;              // loop thread: re-open the current default endpoint
  virtual void watch(std::function<void(bool default_changed)> on_change) = 0;
};

std::unique_ptr<AudioEndpointProvider> make_core_audio(AudioFlow flow);

}  // namespace ld
