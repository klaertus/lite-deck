#include <doctest/doctest.h>
#include <vector>
#include "fake_loop.h"

using namespace ld;

TEST_CASE("FakeLoop advance fires one-shot and periodic timers at their deadlines") {
  FakeLoop loop;
  std::vector<TimePoint> once, periodic;
  loop.at(TimePoint{} + Duration(250), [&] { once.push_back(loop.now()); });
  loop.every(Duration(100), [&] { periodic.push_back(loop.now()); });
  loop.advance(Duration(1000));
  REQUIRE(once.size() == 1);
  CHECK(once[0] == TimePoint{} + Duration(250));
  REQUIRE(periodic.size() == 10);
  for (int i = 0; i < 10; ++i) CHECK(periodic[i] == TimePoint{} + Duration(100 * (i + 1)));
  CHECK(loop.now() == TimePoint{} + Duration(1000));
}

TEST_CASE("FakeLoop runs posts made from a timer callback within the same advance") {
  FakeLoop loop;
  int n = 0;
  loop.at(TimePoint{} + Duration(50), [&] { loop.post([&] { ++n; }); });
  loop.advance(Duration(100));
  CHECK(n == 1);
  CHECK(loop.posted.empty());
}

TEST_CASE("FakeLoop signal calls handlers; a handler may remove itself") {
  FakeLoop loop;
  int a = 0, h = 0;
  void* key = &h;
  loop.add_handle(key, [&] { ++a; loop.remove_handle(key); });
  loop.signal(key);
  CHECK(a == 1);
  loop.signal(key);
  CHECK(a == 1);
  CHECK(loop.handles.empty());
}
