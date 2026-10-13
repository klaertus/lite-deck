#include <doctest/doctest.h>
#include <windows.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include "core/event_loop.h"
#include "web/event_hub.h"
#include "web/loop_bridge.h"

using namespace std::chrono_literals;

TEST_CASE("LoopBridge runs work on the loop thread and returns its result") {
  ld::EventLoop loop;
  ld::LoopBridge bridge(loop);
  const DWORD loop_thread = GetCurrentThreadId();
  std::optional<DWORD> ran_on;
  std::thread t([&] {
    ran_on = bridge.call([] { return GetCurrentThreadId(); });
    loop.quit();
  });
  loop.run();
  t.join();
  REQUIRE(ran_on);
  CHECK(*ran_on == loop_thread);
}

TEST_CASE("LoopBridge gives up after its timeout when the loop does not answer") {
  ld::EventLoop loop;  // never run
  ld::LoopBridge bridge(loop);
  const auto t0 = ld::Clock::now();
  CHECK_FALSE(bridge.call([] { return 1; }, ld::Duration(50)));
  CHECK(ld::Clock::now() - t0 >= 45ms);
}

TEST_CASE("LoopBridge::close wakes a waiting web thread at once") {
  ld::EventLoop loop;  // never run: the call can only end by timeout or close()
  ld::LoopBridge bridge(loop);
  std::optional<int> r = 0;
  ld::Clock::duration waited{};
  std::thread t([&] {
    const auto t0 = ld::Clock::now();
    r = bridge.call([] { return 1; }, ld::Duration(10000));
    waited = ld::Clock::now() - t0;
  });
  std::this_thread::sleep_for(50ms);
  bridge.close();
  t.join();
  CHECK_FALSE(r);
  CHECK(waited < 2s);
  const auto t1 = ld::Clock::now();
  CHECK_FALSE(bridge.call([] { return 1; }, ld::Duration(10000)));  // closed: answers immediately
  CHECK(ld::Clock::now() - t1 < 1s);
}

TEST_CASE("LoopBridge answers nothing at once when the loop is gone") {
  std::unique_ptr<ld::LoopBridge> bridge;
  {
    ld::EventLoop loop;
    bridge = std::make_unique<ld::LoopBridge>(loop);
  }
  const auto t0 = ld::Clock::now();
  CHECK_FALSE(bridge->call([] { return 1; }, ld::Duration(10000)));
  CHECK(ld::Clock::now() - t0 < 1s);
}

TEST_CASE("LoopBridge reports an exception on the loop as no answer, without waiting") {
  ld::EventLoop loop;
  ld::LoopBridge bridge(loop);
  std::optional<int> r = 5;
  ld::Clock::duration waited{};
  std::thread t([&] {
    const auto t0 = ld::Clock::now();
    r = bridge.call([]() -> int { throw std::runtime_error("boom"); }, ld::Duration(10000));
    waited = ld::Clock::now() - t0;
    loop.quit();
  });
  loop.run();
  t.join();
  CHECK_FALSE(r);
  CHECK(waited < 2s);
}

TEST_CASE("EventHub does nothing without streams, and a new stream gets no backlog") {
  ld::EventHub hub;
  hub.publish("device", {{"state", "connected"}});
  hub.frame(0);
  CHECK(hub.streams() == 0);
  auto s = hub.subscribe();
  REQUIRE(s);
  CHECK(hub.streams() == 1);
  CHECK(s->next(ld::Duration(30)) == ": ping\n\n");
}

TEST_CASE("EventHub delivers the latest value of each event to every stream") {
  ld::EventHub hub;
  auto a = hub.subscribe();
  auto b = hub.subscribe();
  REQUIRE(a);
  REQUIRE(b);
  hub.publish("device", {{"state", "busy"}});
  hub.publish("device", {{"state", "connected"}});
  hub.publish("config", {{"revision", 3}});
  const std::string expected =
      "event: config\ndata: {\"revision\":3}\n\nevent: device\ndata: {\"state\":\"connected\"}\n\n";
  CHECK(a->next(ld::Duration(1000)) == expected);
  CHECK(b->next(ld::Duration(1000)) == expected);
}

TEST_CASE("EventHub coalesces frames and sends them at most every 200 ms") {
  ld::EventHub hub;
  auto s = hub.subscribe();
  hub.frame(3);
  hub.frame(3);
  hub.frame(-1);
  CHECK(s->next(ld::Duration(1000)) == "event: frame\ndata: {\"keys\":[3],\"strip\":true}\n\n");
  hub.frame(1);
  const auto t0 = ld::Clock::now();
  CHECK(s->next(ld::Duration(1000)) == "event: frame\ndata: {\"keys\":[1],\"strip\":false}\n\n");
  CHECK(ld::Clock::now() - t0 >= 150ms);
}

TEST_CASE("EventHub accepts two streams at most") {
  ld::EventHub hub;
  auto a = hub.subscribe();
  auto b = hub.subscribe();
  CHECK(a);
  CHECK(b);
  CHECK_FALSE(hub.subscribe());
  a.reset();
  CHECK(hub.streams() == 1);
  CHECK(hub.subscribe());
}

TEST_CASE("an event wakes a waiting stream at once") {
  ld::EventHub hub;
  auto s = hub.subscribe();
  std::string got;
  std::thread t([&] { got = s->next(ld::Duration(10000)); });
  std::this_thread::sleep_for(50ms);
  const auto t0 = ld::Clock::now();
  hub.publish("device", {{"state", "disconnected"}});
  t.join();
  CHECK(got == "event: device\ndata: {\"state\":\"disconnected\"}\n\n");
  CHECK(ld::Clock::now() - t0 < 1s);
}

TEST_CASE("EventHub::close ends a waiting stream at once") {
  ld::EventHub hub;
  auto s = hub.subscribe();
  std::string got = "x";
  std::thread t([&] { got = s->next(ld::Duration(10000)); });
  std::this_thread::sleep_for(50ms);
  hub.close();
  t.join();
  CHECK(got.empty());
  CHECK_FALSE(hub.subscribe());
}

TEST_CASE("EventHub pings at least every 5 s, so a closed page frees its thread soon") {
  CHECK(ld::EventHub::kPing <= ld::Duration(5000));
  ld::EventHub hub;
  auto s = hub.subscribe();
  const auto t0 = ld::Clock::now();
  CHECK(s->next() == ": ping\n\n");
  CHECK(ld::Clock::now() - t0 < 6s);
}

TEST_CASE("EventHub calls on_idle once the last stream ends, not before") {
  ld::EventHub hub;
  int idle = 0;
  hub.on_idle([&] { ++idle; });
  auto a = hub.subscribe();
  auto b = hub.subscribe();
  a.reset();
  CHECK(idle == 0);
  b.reset();
  CHECK(idle == 1);
  auto c = hub.subscribe();
  c.reset();
  CHECK(idle == 2);
}
