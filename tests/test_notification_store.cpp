#include <doctest/doctest.h>
#include "modules/notifications/notification_store.h"

using namespace ld;

static const TimePoint T0{std::chrono::milliseconds(1000)};

static IncomingNotification n(std::uint32_t id, const wchar_t* app = L"App", const wchar_t* t = L"T", const wchar_t* b = L"B") {
  return IncomingNotification{id, app, t, b};
}

TEST_CASE("sync adds new notifications and reports them") {
  NotificationStore s;
  auto added = s.sync({n(1), n(2)}, T0);
  CHECK(added.size() == 2);
  CHECK(s.count() == 2);
  CHECK(s.sync({n(1), n(2)}, T0).empty());
}

TEST_CASE("sync drops notifications that disappeared from Windows") {
  NotificationStore s;
  s.sync({n(1), n(2), n(3)}, T0);
  s.sync({n(2)}, T0);
  CHECK(s.count() == 1);
  CHECK(s.contains(2));
  CHECK_FALSE(s.contains(1));
}

TEST_CASE("current is the most recently first-seen, even within one sync") {
  NotificationStore s;
  s.sync({n(10), n(5)}, T0);
  REQUIRE(s.current());
  CHECK(s.current()->id == 5);  // second in the batch = newer seq
  s.sync({n(10), n(5), n(7)}, T0);
  CHECK(s.current()->id == 7);
}

TEST_CASE("remove acknowledges by id and advances to the next newest") {
  NotificationStore s;
  s.sync({n(1), n(2), n(3)}, T0);
  CHECK(s.remove(3));
  CHECK(s.current()->id == 2);
  CHECK_FALSE(s.remove(3));
  s.remove(2);
  s.remove(1);
  CHECK_FALSE(s.current());
  CHECK(s.count() == 0);
}

TEST_CASE("an acknowledged notification that Windows still lists is not re-added") {
  NotificationStore s;
  s.sync({n(1), n(2)}, T0);
  s.remove(2);
  auto added = s.sync({n(1), n(2)}, T0);  // Windows has not processed the removal yet
  CHECK(added.empty());
  CHECK(s.count() == 1);
  s.sync({n(1)}, T0);  // removal processed: forget the tombstone
  s.sync({n(1), n(2)}, T0);
  CHECK(s.count() == 2);  // a later notification reusing id 2 is accepted again
}

TEST_CASE("store dates each notification from its age") {
  ld::NotificationStore st;
  ld::TimePoint now{std::chrono::milliseconds(100000)};
  ld::IncomingNotification in{7, L"App", L"T", L"B"};
  in.age = std::chrono::milliseconds(40000);
  st.sync({in}, now);
  REQUIRE(st.current());
  CHECK(st.current()->created == ld::TimePoint{std::chrono::milliseconds(60000)});
}
