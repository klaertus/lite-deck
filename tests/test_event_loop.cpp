#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include "core/event_loop.h"
#include "core/log.h"
#include "fake_loop.h"
#include "modules/poster.h"

using namespace std::chrono_literals;

TEST_CASE("post from another thread runs on the loop and quit stops it") {
  ld::EventLoop loop;
  DWORD loop_thread = GetCurrentThreadId();
  DWORD ran_on = 0;
  std::thread t([&] {
    loop.post([&] { ran_on = GetCurrentThreadId(); });
    loop.quit(7);
  });
  int code = loop.run();
  t.join();
  CHECK(code == 7);
  CHECK(ran_on == loop_thread);
}

TEST_CASE("timers fire at their deadline with high resolution") {
  ld::EventLoop loop;
  auto start = ld::Clock::now();
  ld::TimePoint fired{};
  loop.at(start + 30ms, [&] { fired = ld::Clock::now(); loop.quit(); });
  loop.run();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fired - start).count();
  CHECK(elapsed >= 29);
  CHECK(elapsed < 60);
}

TEST_CASE("registered handles call back when signaled and can be removed") {
  ld::EventLoop loop;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  int calls = 0;
  loop.add_handle(ev, [&] {
    ++calls;
    loop.remove_handle(ev);
    loop.at(ld::Clock::now() + 20ms, [&] { loop.quit(); });
  });
  std::thread t([&] { SetEvent(ev); });
  loop.run();
  t.join();
  CHECK(calls == 1);
  CloseHandle(ev);
}

TEST_CASE("exceptions from posted callbacks are contained") {
  ld::EventLoop loop;
  bool after = false;
  loop.post([] { throw std::runtime_error("boom"); });
  loop.post([&] { after = true; loop.quit(3); });
  CHECK(loop.run() == 3);
  CHECK(after);
}

TEST_CASE("an invalid handle is dropped instead of spinning") {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "ld_loop_invalid";
  fs::remove_all(dir);
  ld::log_init(dir / "loop.log", ld::LogLevel::Info);
  ld::EventLoop loop;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  CloseHandle(ev);  // now invalid
  int calls = 0;
  loop.add_handle(ev, [&] { ++calls; });
  loop.at(ld::Clock::now() + 50ms, [&] { loop.quit(5); });
  CHECK(loop.run() == 5);
  CHECK(calls == 0);
  ld::log_init({}, ld::LogLevel::Info);
  std::string text;
  {
    std::ifstream f(dir / "loop.log");
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  fs::remove_all(dir);
  CHECK(text.find("dropped 1 invalid handle(s)") != std::string::npos);
}

TEST_CASE("a poster used after its loop is destroyed is a no-op") {
  auto target = std::make_shared<ld::PostTarget>();
  int ran = 0;
  target->run = [&](const std::function<void()>& f) { f(); };
  ld::Poster p;
  {
    ld::EventLoop loop;
    p = ld::Poster(loop, target);
  }
  CHECK_NOTHROW(p.post([&] { ++ran; }));
  CHECK(ran == 0);
}

TEST_CASE("a post accepted while alive is dropped if the slot dies before the drain") {
  FakeLoop loop;
  auto target = std::make_shared<ld::PostTarget>();
  int ran = 0;
  target->run = [&](const std::function<void()>& f) { f(); };
  ld::Poster p(loop, target);
  p.post([&] { ++ran; });
  target->alive.store(false);  // slot detached before the loop drains
  loop.drain();
  CHECK(ran == 0);
}

TEST_CASE("the loop gate is cleared when the loop is destroyed") {
  std::shared_ptr<ld::LoopGate> g;
  {
    ld::EventLoop loop;
    g = loop.gate();
    CHECK(g->loop == &loop);
  }
  CHECK(g->loop == nullptr);
  std::shared_ptr<ld::LoopGate> f;
  {
    FakeLoop loop;
    f = loop.gate();
    CHECK(f->loop == &loop);
  }
  CHECK(f->loop == nullptr);
}
