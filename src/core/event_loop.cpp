#include "core/event_loop.h"
#include "core/log.h"
#include <windows.h>
#include <algorithm>
#include <stdexcept>

namespace ld {

namespace {
void run_guarded(const std::function<void()>& fn, const char* what) {
  try {
    fn();
  } catch (const std::exception& e) {
    ld::log_error("{} threw: {}", what, e.what());
  } catch (...) {
    ld::log_error("{} threw a non-standard exception", what);
  }
}
}  // namespace

EventLoop::EventLoop() {
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  if (!wake_ || !timer_) throw std::runtime_error("EventLoop: cannot create wait objects");
  gate_->loop = this;
}

EventLoop::~EventLoop() {
  {
    std::lock_guard lock(gate_->mu);
    gate_->loop = nullptr;
  }
  CloseHandle(timer_);
  CloseHandle(wake_);
}

void EventLoop::post(std::function<void()> fn) {
  {
    std::lock_guard lock(mu_);
    queue_.push_back(std::move(fn));
  }
  SetEvent(wake_);
}

void EventLoop::add_handle(void* handle, std::function<void()> on_signaled) {
  remove_handle(handle);
  handles_.emplace_back(handle, std::move(on_signaled));
}

void EventLoop::remove_handle(void* handle) {
  std::erase_if(handles_, [&](const auto& p) { return p.first == handle; });
}

void EventLoop::quit(int code) {
  post([this, code] {
    running_ = false;
    exit_code_ = code;
  });
}

void EventLoop::arm_timer() {
  auto deadline = sched_.next_deadline();
  if (!deadline) {
    CancelWaitableTimer(timer_);
    return;
  }
  auto due = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - Clock::now()).count() / 100;
  LARGE_INTEGER li;
  li.QuadPart = -std::max<long long>(1, due);  // relative, 100 ns units
  SetWaitableTimerEx(timer_, &li, 0, nullptr, nullptr, nullptr, 0);
}

void EventLoop::drain_queue() {
  std::vector<std::function<void()>> q;
  {
    std::lock_guard lock(mu_);
    q.swap(queue_);
  }
  for (auto& f : q) run_guarded(f, "posted callback");
}

int EventLoop::run() {
  running_ = true;
  while (running_) {
    arm_timer();
    std::vector<HANDLE> hs{wake_, timer_};
    for (const auto& [h, fn] : handles_) hs.push_back(h);
    DWORD r = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(hs.size()), hs.data(), INFINITE, QS_ALLINPUT,
                                          MWMO_INPUTAVAILABLE);
    if (r == WAIT_FAILED) {
      DWORD err = GetLastError();
      std::size_t before = handles_.size();
      std::erase_if(handles_, [](const auto& p) { return WaitForSingleObject(p.first, 0) == WAIT_FAILED; });
      log_error("wait failed (error {}); dropped {} invalid handle(s)", err, before - handles_.size());
      if (before == handles_.size()) {
        running_ = false;
        exit_code_ = 1;
      }
      continue;
    }
    if (r >= WAIT_OBJECT_0 + 2 && r < WAIT_OBJECT_0 + hs.size()) {
      HANDLE h = hs[r - WAIT_OBJECT_0];
      auto it = std::find_if(handles_.begin(), handles_.end(), [&](const auto& p) { return p.first == h; });
      if (it != handles_.end()) {
        auto fn = it->second;
        run_guarded(fn, "handle callback");
      }
    }
    drain_queue();
    sched_.run_due(Clock::now());
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running_ = false;
        exit_code_ = static_cast<int>(msg.wParam);
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return exit_code_;
}

}  // namespace ld
