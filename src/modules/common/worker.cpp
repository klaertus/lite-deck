// src/modules/common/worker.cpp
#include "modules/common/worker.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include "core/log.h"

namespace ld {
namespace {

struct Queue {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool stop = false;
  bool started = false;
};

void work(const std::shared_ptr<Queue>& q) {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(q->mu);
      q->cv.wait(lock, [&] { return q->stop || !q->jobs.empty(); });
      if (q->stop) return;
      job = std::move(q->jobs.front());
      q->jobs.pop_front();
    }
    try {
      job();
    } catch (const std::exception& e) {
      log_error("module worker: a job failed: {}", e.what());
    } catch (...) {
      log_error("module worker: a job failed");
    }
  }
}

// Owned by the runner's copies: the last one stops the thread.
struct Owner {
  std::shared_ptr<Queue> q = std::make_shared<Queue>();
  Owner() = default;
  Owner(const Owner&) = delete;
  Owner& operator=(const Owner&) = delete;
  ~Owner() {
    std::deque<std::function<void()>> dropped;
    {
      std::lock_guard lock(q->mu);
      q->stop = true;
      dropped.swap(q->jobs);  // destroyed here, outside the lock
    }
    q->cv.notify_all();
  }
};

}  // namespace

Runner worker_runner() {
  auto owner = std::make_shared<Owner>();
  return [owner](std::function<void()> job) {
    const std::shared_ptr<Queue>& q = owner->q;
    {
      std::lock_guard lock(q->mu);
      if (q->stop) return;
      q->jobs.push_back(std::move(job));
      if (!q->started) {
        q->started = true;
        std::thread([q] { work(q); }).detach();  // nobody joins: see worker.h
      }
    }
    q->cv.notify_one();
  };
}

}  // namespace ld
