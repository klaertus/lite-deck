// tests/test_worker.cpp
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include "modules/common/worker.h"

using namespace std::chrono_literals;

TEST_CASE("worker_runner runs jobs in order on another thread") {
  ld::Runner run = ld::worker_runner();
  std::vector<int> order;
  std::promise<std::thread::id> done;
  run([&] { order.push_back(1); });
  run([&] { order.push_back(2); });
  run([&] { done.set_value(std::this_thread::get_id()); });
  auto f = done.get_future();
  REQUIRE(f.wait_for(5s) == std::future_status::ready);
  CHECK(f.get() != std::this_thread::get_id());
  CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("destroying a runner does not wait for the job in flight, and drops the queued ones") {
  auto release = std::make_shared<std::promise<void>>();
  auto started = std::make_shared<std::promise<void>>();
  auto second_ran = std::make_shared<std::atomic<bool>>(false);
  {
    ld::Runner run = ld::worker_runner();
    std::shared_future<void> gate = release->get_future().share();
    run([started, gate] {
      started->set_value();
      gate.wait();
    });
    run([second_ran] { second_ran->store(true); });
    REQUIRE(started->get_future().wait_for(5s) == std::future_status::ready);
    const auto t0 = std::chrono::steady_clock::now();
    run = nullptr;  // the job in flight is still blocked
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
  }
  release->set_value();
  std::this_thread::sleep_for(50ms);
  CHECK_FALSE(second_ran->load());
}
