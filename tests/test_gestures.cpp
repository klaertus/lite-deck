#include <doctest/doctest.h>
#include "layout/gestures.h"

using namespace ld;

static TimePoint T(int ms) { return TimePoint{} + Duration(ms); }

TEST_CASE("quick touch without movement is a tap at the start position") {
  GestureRecognizer g;
  CHECK_FALSE(g.down(300, T(0)));
  CHECK_FALSE(g.move(305, T(30)));
  auto r = g.up(T(150));
  REQUIRE(r);
  CHECK(r->kind == GestureKind::Tap);
  CHECK(r->start_x == 300);
}

TEST_CASE("a slow but still tap (450 ms) is still a tap") {
  GestureRecognizer g;
  g.down(100, T(0));
  g.move(102, T(440));
  auto r = g.up(T(450));
  REQUIRE(r);
  CHECK(r->kind == GestureKind::Tap);
}

TEST_CASE("holding still fires LongTouch once and no tap on release") {
  GestureRecognizer g;
  g.down(500, T(0));
  CHECK_FALSE(g.move(500, T(300)));
  auto l = g.move(501, T(520));
  REQUIRE(l);
  CHECK(l->kind == GestureKind::LongTouch);
  CHECK(l->start_x == 500);
  CHECK_FALSE(g.move(500, T(700)));
  CHECK_FALSE(g.up(T(900)));
}

TEST_CASE("horizontal movement of 40 px or more is a swipe in that direction") {
  GestureRecognizer g;
  g.down(100, T(0));
  g.move(200, T(100));
  auto r = g.up(T(200));
  REQUIRE(r);
  CHECK(r->kind == GestureKind::SwipeRight);
  CHECK(r->start_x == 100);

  g.down(700, T(1000));
  g.move(600, T(1100));
  auto l = g.up(T(1150));
  REQUIRE(l);
  CHECK(l->kind == GestureKind::SwipeLeft);
}

TEST_CASE("movement between tolerance and swipe threshold produces nothing") {
  GestureRecognizer g;
  g.down(100, T(0));
  g.move(130, T(50));
  CHECK_FALSE(g.up(T(100)));
}

TEST_CASE("gestures ignore incomplete sequences") {
  GestureRecognizer g;
  CHECK_FALSE(g.up(T(0)));                 // up without down
  CHECK_FALSE(g.move(50, T(10)));          // move without down starts a touch implicitly
  auto r = g.up(T(60));
  REQUIRE(r);
  CHECK(r->kind == GestureKind::Tap);
  CHECK(r->start_x == 50);
  g.down(10, T(100));
  g.down(400, T(120));                     // second down restarts the touch
  auto r2 = g.up(T(150));
  REQUIRE(r2);
  CHECK(r2->start_x == 400);
  CHECK_FALSE(g.up(T(160)));               // double up
}

TEST_CASE("a stationary hold released after 500 ms with no moves is a LongTouch") {
  GestureRecognizer g;
  g.down(200, T(0));
  auto r = g.up(T(700));
  REQUIRE(r);
  CHECK(r->kind == GestureKind::LongTouch);
  CHECK(r->start_x == 200);

  g.down(200, T(1000));
  auto t = g.up(T(1499));
  REQUIRE(t);
  CHECK(t->kind == GestureKind::Tap);
}

TEST_CASE("gesture limits: 19 px is still a tap, 20 px is not") {
  GestureRecognizer g;
  g.down(100, T(0));
  g.move(119, T(50));
  auto t = g.up(T(100));
  REQUIRE(t);
  CHECK(t->kind == GestureKind::Tap);
  g.down(100, T(1000));
  g.move(120, T(1050));
  CHECK_FALSE(g.up(T(1100)));
}

TEST_CASE("gesture limits: 39 px is not a swipe, 40 px is, in both directions") {
  GestureRecognizer g;
  g.down(100, T(0));
  g.move(139, T(50));
  CHECK_FALSE(g.up(T(100)));
  g.down(100, T(1000));
  g.move(140, T(1050));
  auto r = g.up(T(1100));
  REQUIRE(r);
  CHECK(r->kind == GestureKind::SwipeRight);
  g.down(100, T(2000));
  g.move(60, T(2050));
  auto l = g.up(T(2100));
  REQUIRE(l);
  CHECK(l->kind == GestureKind::SwipeLeft);
}

TEST_CASE("gesture limits: LongTouch at exactly 500 ms, not at 499 ms") {
  GestureRecognizer g;
  g.down(300, T(0));
  CHECK_FALSE(g.move(300, T(499)));
  auto l = g.move(300, T(500));
  REQUIRE(l);
  CHECK(l->kind == GestureKind::LongTouch);
}

TEST_CASE("gesture limits: a hold that drifted 19 px is a LongTouch, 20 px is nothing") {
  GestureRecognizer g;
  g.down(300, T(0));
  g.move(319, T(100));
  auto l = g.move(319, T(600));
  REQUIRE(l);
  CHECK(l->kind == GestureKind::LongTouch);
  g.down(300, T(1000));
  g.move(320, T(1100));
  CHECK_FALSE(g.move(320, T(1600)));
  CHECK_FALSE(g.up(T(1700)));
}

TEST_CASE("gesture limits: a swipe that comes back to its start is nothing") {
  GestureRecognizer g;
  g.down(100, T(0));
  g.move(200, T(50));
  g.move(105, T(100));
  CHECK_FALSE(g.up(T(150)));
}
