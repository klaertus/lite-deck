// src/modules/common/worker.h: somewhere other than the loop thread for the system calls that may
// block (ICMP, name resolution, PDH, the free space of a sleeping drive).
#pragma once
#include <functional>

namespace ld {

// Runs a job away from the loop thread. A job must not touch its module: it calls what it captured
// (a reader held by value or by shared_ptr) and posts its result back through a Poster captured by
// value, which drops it if the module is gone.
using Runner = std::function<void(std::function<void()> job)>;

// One worker thread per runner, started on the first job; jobs run one at a time, in order. When the
// last copy of the runner is destroyed, the queued jobs are dropped and the thread ends after the job
// in flight, without anyone waiting for it: destroying a module never blocks the loop.
Runner worker_runner();

}  // namespace ld
