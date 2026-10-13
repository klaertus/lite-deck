// src/core/event_loop.h
#pragma once
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "core/loop_api.h"
#include "core/scheduler.h"

namespace ld {

// Single-threaded loop: MsgWaitForMultipleObjectsEx on {wake event, high-resolution timer, I/O handles}
// plus the thread's window messages.
class EventLoop final : public LoopApi {
public:
  EventLoop();
  ~EventLoop() override;
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  TimePoint now() const override { return Clock::now(); }
  TimerId at(TimePoint when, std::function<void()> fn) override { return sched_.at(when, std::move(fn)); }
  TimerId every(Duration period, std::function<void()> fn) override { return sched_.every(period, std::move(fn), now()); }
  void cancel(TimerId id) override { sched_.cancel(id); }
  void post(std::function<void()> fn) override;
  void add_handle(void* handle, std::function<void()> on_signaled) override;
  void remove_handle(void* handle) override;
  std::shared_ptr<LoopGate> gate() override { return gate_; }

  int run();
  void quit(int code = 0);  // thread-safe

private:
  void arm_timer();
  void drain_queue();

  std::shared_ptr<LoopGate> gate_ = std::make_shared<LoopGate>();
  Scheduler sched_;
  void* wake_ = nullptr;
  void* timer_ = nullptr;
  std::mutex mu_;
  std::vector<std::function<void()>> queue_;
  std::vector<std::pair<void*, std::function<void()>>> handles_;
  bool running_ = false;
  int exit_code_ = 0;
};

}  // namespace ld
