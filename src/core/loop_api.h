// src/core/loop_api.h
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include "core/types.h"

namespace ld {

class LoopApi;

// Owned by a loop and shared with every Poster. The loop clears `loop` (under `mu`) when it is
// destroyed, so a late post from another thread becomes a no-op instead of a use-after-free.
struct LoopGate {
  std::mutex mu;
  LoopApi* loop = nullptr;
};

// What components need from the event loop. Implemented by EventLoop (real) and FakeLoop (tests).
// Everything except post() must be called on the loop thread.
class LoopApi {
public:
  virtual ~LoopApi() = default;
  virtual TimePoint now() const = 0;
  virtual TimerId at(TimePoint when, std::function<void()> fn) = 0;
  virtual TimerId every(Duration period, std::function<void()> fn) = 0;
  virtual void cancel(TimerId id) = 0;
  virtual void post(std::function<void()> fn) = 0;  // thread-safe
  virtual void add_handle(void* handle, std::function<void()> on_signaled) = 0;
  virtual void remove_handle(void* handle) = 0;
  virtual std::shared_ptr<LoopGate> gate() = 0;
};

}  // namespace ld
