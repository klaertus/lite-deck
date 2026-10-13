// tests/test_shutdown.cpp: the daemon's exit stays short when work in flight is stuck. Fakes only:
// the blocked "requests" and "launches" wait on a gate the test opens at the end.
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include "app/shutdown_watchdog.h"
#include "core/event_loop.h"
#include "core/http.h"
#include "modules/common/worker.h"

using namespace std::chrono_literals;

namespace {

// Blocks every caller until open(), whatever happens meanwhile (a transport that ignores the abort,
// a launch stuck in the shell). Shared by value with the threads it blocks, which may outlive the test.
struct Gate {
  std::mutex mu;
  std::condition_variable cv;
  bool opened = false;
  int waiting = 0;
  void wait() {
    std::unique_lock lock(mu);
    ++waiting;
    cv.notify_all();
    cv.wait(lock, [&] { return opened; });
  }
  bool wait_for_waiters(int n, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, timeout, [&] { return waiting >= n; });
  }
  void open() {
    {
      std::lock_guard lock(mu);
      opened = true;
    }
    cv.notify_all();
  }
};

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("shutdown: blocked http workers and module workers are given up within the budget") {
  auto gate = std::make_shared<Gate>();
  std::atomic<int> answered{0};
  ld::EventLoop loop;
  double elapsed = 0;
  {
    // Every request blocks and ignores the abort: the worst case of a WinHTTP call stuck in DNS or proxy discovery.
    auto http = std::make_unique<ld::WinHttpClient>(loop, [gate](const ld::HttpRequest& r, std::atomic<void*>*) {
      gate->wait();
      return ld::HttpResponse{200, r.url, ""};
    });
    http->get("https://weather/forecast", [&](ld::HttpResponse) { ++answered; });
    for (int i = 0; i < ld::WinHttpClient::kIsolatedWorkers; ++i)
      http->get(ld::HttpRequest{.url = "http://sensor/" + std::to_string(i), .lane = ld::HttpLane::Isolated},
                [&](ld::HttpResponse) { ++answered; });
    // A launcher key's worker and a ping key's worker, each stuck in its job.
    ld::Runner launch = ld::worker_runner();
    ld::Runner ping = ld::worker_runner();
    launch([gate] { gate->wait(); });
    ping([gate] { gate->wait(); });
    const int blocked = 1 + ld::WinHttpClient::kIsolatedWorkers + 2;
    REQUIRE(gate->wait_for_waiters(blocked, 5s));

    // The teardown of main(): the modules (their runners) go first, then the http client.
    const auto t0 = std::chrono::steady_clock::now();
    launch = nullptr;
    ping = nullptr;
    http.reset();
    elapsed = seconds_since(t0);
  }
  MESSAGE("shutdown with " << 1 + ld::WinHttpClient::kIsolatedWorkers + 2 << " blocked workers took " << elapsed << " s");
  CHECK(elapsed >= 0.9);  // the client gave its workers kStopWait to end...
  CHECK(elapsed < 2.0);   // ...and not more
  gate->open();           // the left-over threads now end; none may post to the loop
  loop.at(ld::Clock::now() + 200ms, [&] { loop.quit(0); });
  loop.run();
  CHECK(answered == 0);
}

TEST_CASE("shutdown: idle http workers are joined at once") {
  ld::EventLoop loop;
  int answered = 0;
  double elapsed = 0;
  {
    auto http = std::make_unique<ld::WinHttpClient>(loop, [](const ld::HttpRequest& r, std::atomic<void*>*) {
      return ld::HttpResponse{200, r.url, ""};
    });
    http->get("https://prices/now", [&](ld::HttpResponse) {
      ++answered;
      loop.quit(0);
    });
    http->get(ld::HttpRequest{.url = "http://sensor/x", .lane = ld::HttpLane::Isolated}, [](ld::HttpResponse) {});
    loop.at(ld::Clock::now() + 5s, [&] { loop.quit(1); });
    CHECK(loop.run() == 0);
    const auto t0 = std::chrono::steady_clock::now();
    http.reset();
    elapsed = seconds_since(t0);
  }
  CHECK(answered == 1);
  CHECK(elapsed < 0.3);
}

TEST_CASE("shutdown: the watchdog ends a stuck exit after its budget, and only then") {
  auto stuck = std::make_shared<std::promise<void>>();
  std::shared_future<void> released = stuck->get_future().share();
  std::atomic<int> fired{0};
  ld::ShutdownWatchdog watchdog([&fired, stuck] {
    ++fired;
    stuck->set_value();  // what TerminateProcess does to the stuck step, in the test
  });
  const auto t0 = std::chrono::steady_clock::now();
  watchdog.arm(300ms);
  watchdog.arm(10s);  // ignored: the first budget counts
  REQUIRE(released.wait_for(5s) == std::future_status::ready);  // a teardown step blocked "forever"
  const double elapsed = seconds_since(t0);
  CHECK(elapsed >= 0.25);
  CHECK(elapsed < 2.0);
  std::this_thread::sleep_for(100ms);
  CHECK(fired == 1);
  CHECK(ld::kForcedExitCode != 0);  // a forced end never looks like a clean exit
}

TEST_CASE("shutdown: a disarmed or destroyed watchdog never fires") {
  auto fired = std::make_shared<std::atomic<int>>(0);
  {
    ld::ShutdownWatchdog disarmed([fired] { ++*fired; });
    disarmed.arm(100ms);
    disarmed.disarm();
    ld::ShutdownWatchdog destroyed([fired] { ++*fired; });
    destroyed.arm(100ms);
  }
  ld::ShutdownWatchdog never_armed([fired] { ++*fired; });
  std::this_thread::sleep_for(300ms);
  CHECK(*fired == 0);
}
