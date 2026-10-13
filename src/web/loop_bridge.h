// src/web/loop_bridge.h
#pragma once
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include "core/log.h"
#include "core/loop_api.h"

namespace ld {

// How a web thread gets something done on the loop thread: call() posts `fn` there and waits for its
// result, at most `timeout`. Everything the daemon owns (Deck, modules, ConfigService, StateStore) is
// only touched inside `fn`, on the loop; the result is copied out. close() (shutdown) wakes every
// waiter at once, so a web thread never holds the exit up.
class LoopBridge {
public:
  static constexpr Duration kTimeout{2000};

  explicit LoopBridge(LoopApi& loop) : gate_(loop.gate()) {}
  LoopBridge(const LoopBridge&) = delete;
  LoopBridge& operator=(const LoopBridge&) = delete;

  // nullopt: timeout, loop gone, bridge closed, or `fn` threw (logged). `fn` may still run after a
  // timeout: capture only what outlives the loop, or copies.
  template <class F>
  std::optional<std::invoke_result_t<F&>> call(F fn, Duration timeout = kTimeout) {
    using R = std::invoke_result_t<F&>;
    struct Result {
      std::optional<R> value;
      bool done = false;
    };
    auto result = std::make_shared<Result>();
    auto shared = shared_;
    {
      std::lock_guard gate_lock(gate_->mu);
      if (!gate_->loop) return std::nullopt;
      {
        std::lock_guard lock(shared->mu);
        if (shared->closed) return std::nullopt;
      }
      gate_->loop->post([shared, result, fn = std::move(fn)]() mutable {
        {
          std::lock_guard lock(shared->mu);
          if (shared->closed) return;
        }
        std::optional<R> value;
        try {
          value.emplace(fn());
        } catch (const std::exception& e) {
          log_error("web: work on the loop failed: {}", e.what());
        } catch (...) {
          log_error("web: work on the loop failed");
        }
        std::lock_guard lock(shared->mu);
        result->value = std::move(value);
        result->done = true;
        shared->cv.notify_all();
      });
    }
    std::unique_lock lock(shared->mu);
    shared->cv.wait_for(lock, timeout, [&] { return result->done || shared->closed; });
    if (!result->done) return std::nullopt;
    return std::move(result->value);
  }

  void close() {
    {
      std::lock_guard lock(shared_->mu);
      shared_->closed = true;
    }
    shared_->cv.notify_all();
  }

private:
  struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    bool closed = false;
  };
  std::shared_ptr<LoopGate> gate_;
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

}  // namespace ld
