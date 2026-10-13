#include <doctest/doctest.h>
#include <stdexcept>
#include <vector>
#include "core/scheduler.h"

using namespace ld;
static TimePoint T(int ms) { return TimePoint{} + Duration(ms); }

TEST_CASE("one-shot timers fire once in deadline order") {
  Scheduler s;
  std::vector<int> order;
  s.at(T(20), [&] { order.push_back(2); });
  s.at(T(10), [&] { order.push_back(1); });
  CHECK(s.next_deadline() == T(10));
  s.run_due(T(5));
  CHECK(order.empty());
  s.run_due(T(25));
  CHECK(order == std::vector<int>{1, 2});
  CHECK(s.size() == 0);
  CHECK_FALSE(s.next_deadline());
}

TEST_CASE("periodic timers reschedule and skip missed periods") {
  Scheduler s;
  int n = 0;
  s.every(Duration(100), [&] { ++n; }, T(0));
  CHECK(s.next_deadline() == T(100));
  s.run_due(T(100));
  CHECK(n == 1);
  CHECK(s.next_deadline() == T(200));
  s.run_due(T(1000));  // late: fires once, next is now + period
  CHECK(n == 2);
  CHECK(s.next_deadline() == T(1100));
}

TEST_CASE("cancel, including from inside a callback") {
  Scheduler s;
  int n = 0;
  TimerId id = 0;
  id = s.every(Duration(10), [&] { ++n; s.cancel(id); }, T(0));
  TimerId other = s.at(T(10), [&] { n += 100; });
  s.cancel(other);
  s.run_due(T(50));
  CHECK(n == 1);
  CHECK(s.size() == 0);
}

TEST_CASE("a callback may add timers; new due timers wait for the next run") {
  Scheduler s;
  int n = 0;
  s.at(T(10), [&] { s.at(T(10), [&] { ++n; }); });
  s.run_due(T(10));
  CHECK(n == 0);
  s.run_due(T(10));
  CHECK(n == 1);
}

TEST_CASE("a throwing timer does not stop the others") {
  Scheduler s;
  int n = 0;
  s.at(T(10), [] { throw std::runtime_error("boom"); });
  s.at(T(10), [&] { ++n; });
  s.every(Duration(10), [] { throw 42; }, T(0));
  CHECK_NOTHROW(s.run_due(T(10)));
  CHECK(n == 1);
  CHECK(s.size() == 1);  // the periodic one survives
}
