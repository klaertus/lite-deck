// tests/fake_loop.h
#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include "core/loop_api.h"
#include "core/scheduler.h"

// Deterministic LoopApi: time only moves through advance().
class FakeLoop final : public ld::LoopApi {
public:
  FakeLoop() = default;
  FakeLoop(const FakeLoop&) = delete;
  FakeLoop& operator=(const FakeLoop&) = delete;
  ld::TimePoint t{};
  ld::Scheduler sched;
  std::vector<std::function<void()>> posted;
  std::map<void*, std::function<void()>> handles;

  ld::TimePoint now() const override { return t; }
  ld::TimerId at(ld::TimePoint w, std::function<void()> f) override { return sched.at(w, std::move(f)); }
  ld::TimerId every(ld::Duration p, std::function<void()> f) override { return sched.every(p, std::move(f), t); }
  void cancel(ld::TimerId id) override { sched.cancel(id); }
  void post(std::function<void()> f) override { posted.push_back(std::move(f)); }
  void add_handle(void* h, std::function<void()> f) override { handles[h] = std::move(f); }
  void remove_handle(void* h) override { handles.erase(h); }
  std::shared_ptr<ld::LoopGate> gate_ = [this] {
    auto g = std::make_shared<ld::LoopGate>();
    g->loop = this;
    return g;
  }();
  std::shared_ptr<ld::LoopGate> gate() override { return gate_; }
  ~FakeLoop() override {
    std::lock_guard lock(gate_->mu);
    gate_->loop = nullptr;
  }

  void drain() {
    while (!posted.empty()) {
      auto q = std::move(posted);
      posted.clear();
      for (auto& f : q) f();
    }
  }
  // Steps time to each deadline in order, so periodic timers fire once per period (no catch-up skipping).
  void advance(ld::Duration d) {
    const ld::TimePoint end = t + d;
    drain();
    for (;;) {
      auto n = sched.next_deadline();
      if (!n || *n > end) break;
      t = std::max(t, *n);
      sched.run_due(t);
      drain();
    }
    t = end;
    drain();
  }
  void signal(void* h) {
    auto it = handles.find(h);
    if (it != handles.end()) {
      auto f = it->second;
      f();
    }
    drain();
  }
};
