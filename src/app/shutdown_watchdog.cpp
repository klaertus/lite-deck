// src/app/shutdown_watchdog.cpp
#include "app/shutdown_watchdog.h"
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ld {

struct ShutdownWatchdog::State {
  std::mutex mu;
  std::condition_variable cv;
  bool armed = false;
  bool disarmed = false;
  std::function<void()> on_expired;
};

ShutdownWatchdog::ShutdownWatchdog(std::function<void()> on_expired) : st_(std::make_shared<State>()) {
  st_->on_expired = std::move(on_expired);
}

ShutdownWatchdog::~ShutdownWatchdog() { disarm(); }

void ShutdownWatchdog::arm(Duration budget) noexcept {
  try {
    {
      std::lock_guard lock(st_->mu);
      if (st_->armed || st_->disarmed) return;
      st_->armed = true;
    }
    // Detached: it must still be able to fire while the thread that armed it is the one blocked.
    std::thread([st = st_, budget] {
      std::function<void()> fire;
      {
        std::unique_lock lock(st->mu);
        if (st->cv.wait_for(lock, budget, [&] { return st->disarmed; })) return;
        fire = st->on_expired;
      }
      if (fire) fire();
    }).detach();
  } catch (...) {  // no thread (out of resources): the quit itself must go on
    try {
      std::lock_guard lock(st_->mu);
      st_->armed = false;
    } catch (...) {
    }
    OutputDebugStringW(L"lite-deck: the shutdown watchdog could not start\n");
  }
}

void ShutdownWatchdog::disarm() {
  {
    std::lock_guard lock(st_->mu);
    st_->disarmed = true;
  }
  st_->cv.notify_all();
}

void end_process_now() {
  OutputDebugStringW(L"lite-deck: the exit did not finish in time; ending the process\n");
  TerminateProcess(GetCurrentProcess(), kForcedExitCode);
}

}  // namespace ld
