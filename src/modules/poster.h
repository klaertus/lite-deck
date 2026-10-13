// src/modules/poster.h
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include "core/loop_api.h"

namespace ld {

// Shared by a slot context and every Poster it hands out. `run` is only ever called on the loop thread.
struct PostTarget {
  std::atomic<bool> alive{true};
  std::function<void(const std::function<void()>&)> run;
};

// Copyable handle a module may capture in callbacks that fire on other threads (WinRT, HTTP worker).
// post() is thread-safe; the function runs later on the loop thread, or is dropped if the module was
// destroyed in the meantime.
class Poster {
public:
  Poster() = default;
  Poster(LoopApi& loop, std::shared_ptr<PostTarget> target) : gate_(loop.gate()), target_(std::move(target)) {}
  // Thread-safe. Capture a Poster by value (copy it into each callback); do not share one Poster
  // object between threads while assigning to it.
  void post(std::function<void()> fn) const {
    if (!gate_ || !target_ || !target_->alive.load()) return;
    std::lock_guard lock(gate_->mu);
    if (!gate_->loop) return;
    gate_->loop->post([t = target_, fn = std::move(fn)] {
      if (t->alive.load()) t->run(fn);
    });
  }

private:
  std::shared_ptr<LoopGate> gate_;
  std::shared_ptr<PostTarget> target_;
};

}  // namespace ld
