// src/app/shutdown_watchdog.h: a hard bound on the time the daemon takes to exit once asked to.
#pragma once
#include <functional>
#include <memory>
#include "core/types.h"

namespace ld {

// How long a requested exit may take before the process is ended: a step that blocks (a device
// that no longer answers, a hung shell call in a module's teardown) never keeps an installer or a
// Windows sign-out waiting. A clean exit takes well under a second.
inline constexpr Duration kShutdownBudget{4000};
// The exit code of a process ended by the watchdog: a forced end is never mistaken for a clean one.
inline constexpr unsigned kForcedExitCode = 3;

// Calls `on_expired` once, on its own (detached) thread, when the budget given to the first arm()
// has passed, unless disarm() came first. Destroying the watchdog disarms it.
class ShutdownWatchdog {
public:
  explicit ShutdownWatchdog(std::function<void()> on_expired);
  ~ShutdownWatchdog();
  ShutdownWatchdog(const ShutdownWatchdog&) = delete;
  ShutdownWatchdog& operator=(const ShutdownWatchdog&) = delete;

  // Later calls are ignored. Never throws: if its thread cannot start, the exit goes on unguarded
  // (reported with OutputDebugString) and a later arm() may try again.
  void arm(Duration budget = kShutdownBudget) noexcept;
  void disarm();

private:
  struct State;
  std::shared_ptr<State> st_;
};

// The daemon's action on expiry: ends this process at once (TerminateProcess, kForcedExitCode).
// It may run while static objects are being destroyed, or while the stuck thread holds the log's
// lock, so it never writes to the log: only OutputDebugString, which needs neither.
void end_process_now();

}  // namespace ld
