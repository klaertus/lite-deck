#include <doctest/doctest.h>
#include <atomic>
#include <utility>
#include <windows.h>
#include "fake_context.h"
#include "modules/schema.h"
#include "modules/seek/seek.h"

using namespace ld;
using std::chrono::milliseconds;

namespace {
// Never touches a real media session.
struct FakeTarget : SeekTarget {
  std::function<void(bool)> cb;
  std::optional<SeekTimeline> tl = SeekTimeline{0, 300, 100};
  bool accept = true;
  bool inline_done = true;
  std::vector<double> seeks;
  std::function<void(bool)> pending;  // when !inline_done
  void start(std::function<void(bool)> on_playing) override { cb = std::move(on_playing); }
  std::optional<SeekTimeline> timeline() override { return tl; }
  void seek(double pos, std::function<void(bool)> done) override {
    seeks.push_back(pos);
    if (inline_done) done(accept);
    else pending = std::move(done);
  }
};
struct Rig {
  FakeLoop loop;
  FakeContext ctx{loop};
  FakeTarget* target = nullptr;
  std::unique_ptr<Module> m;
  explicit Rig(int step = 10, bool with_target = true) {
    m = make_seek_module([this, with_target]() -> std::unique_ptr<SeekTarget> {
      if (!with_target) return nullptr;
      auto t = std::make_unique<FakeTarget>();
      target = t.get();
      return t;
    });
    m->start(ctx, nlohmann::json{{"step_s", step}, {"color", "#4cd964"}});
  }
  void turn(int d) { m->on_event({EventKind::Rotate, d}); }
  void advance(int ms) { loop.advance(milliseconds(ms)); }
};
}  // namespace

TEST_CASE("seek accumulator: one detent is due after the window") {
  SeekAccumulator a;
  const TimePoint t0{};
  CHECK_FALSE(a.due());
  a.add(+1, t0);
  REQUIRE(a.due());
  CHECK(*a.due() == t0 + milliseconds(150));
  CHECK(a.take(t0 + milliseconds(149)) == 0);  // quiet time respected
  CHECK(a.take(t0 + milliseconds(150)) == 1);
  CHECK_FALSE(a.due());
  CHECK(a.take(t0 + milliseconds(400)) == 0);
}

TEST_CASE("seek accumulator: a burst adds up, directions cancel out") {
  SeekAccumulator a;
  const TimePoint t0{};
  for (int i = 0; i < 5; ++i) a.add(+1, t0 + milliseconds(i * 25));  // 5 detents within 100 ms
  CHECK(a.take(t0 + milliseconds(100)) == 0);
  CHECK(a.take(t0 + milliseconds(150)) == 5);
  a.add(+1, t0 + milliseconds(200));
  a.add(-1, t0 + milliseconds(220));
  a.add(-1, t0 + milliseconds(240));
  CHECK(a.take(t0 + milliseconds(350)) == -1);
  a.add(+1, t0 + milliseconds(400));
  a.add(-1, t0 + milliseconds(410));
  CHECK(a.take(t0 + milliseconds(550)) == 0);
  CHECK_FALSE(a.due());
}

TEST_CASE("seek_position clamps to the timeline and needs one") {
  CHECK(*seek_position({0, 300, 100}, 50) == doctest::Approx(150));
  CHECK(*seek_position({0, 300, 5}, -30) == doctest::Approx(0));     // start
  CHECK(*seek_position({0, 300, 290}, 30) == doctest::Approx(299));  // 1 s before the end (never EndTime)
  CHECK(*seek_position({0, 0.5, 0.2}, 10) == doctest::Approx(0));    // shorter than 1 s: start
  CHECK(*seek_position({10, 70, 15}, -20) == doctest::Approx(10));   // non-zero start
  CHECK_FALSE(seek_position({0, 0, 0}, 10));
  CHECK_FALSE(seek_position({50, 20, 30}, 10));
}

TEST_CASE("seek knob sends one seek for a burst of detents") {
  Rig r(10);
  for (int i = 0; i < 5; ++i) {
    r.turn(+1);
    r.advance(20);
  }
  CHECK(r.target->seeks.empty());
  r.advance(60);  // 160 ms after the first detent
  REQUIRE(r.target->seeks.size() == 1);
  CHECK(r.target->seeks[0] == doctest::Approx(150));  // 100 + 5 x 10
  r.advance(1000);
  CHECK(r.target->seeks.size() == 1);
}

TEST_CASE("seek knob seeks at most every window while turning, backwards too") {
  Rig r(5);
  for (int i = 0; i < 10; ++i) {  // 10 detents, one every 40 ms
    r.turn(-1);
    r.advance(40);
  }
  r.advance(200);
  REQUIRE(r.target->seeks.size() >= 2);
  CHECK(r.target->seeks.size() <= 4);  // 400 ms of turning / 150 ms
  CHECK(r.target->seeks.back() == doctest::Approx(50));  // 100 - 10 x 5, each seek based on the previous one
}

TEST_CASE("seek knob: opposite detents cancel, press does nothing") {
  Rig r;
  r.turn(+1);
  r.turn(-1);
  r.m->on_event({EventKind::Press});
  r.advance(500);
  CHECK(r.target->seeks.empty());
}

TEST_CASE("seek knob does nothing without a session, a timeline, or when refused") {
  SUBCASE("no session") {
    Rig r;
    r.target->tl.reset();
    r.turn(+1);
    CHECK_NOTHROW(r.advance(200));
    CHECK(r.target->seeks.empty());
  }
  SUBCASE("no timeline") {
    Rig r;
    r.target->tl = SeekTimeline{0, 0, 0};
    r.turn(+1);
    CHECK_NOTHROW(r.advance(200));
    CHECK(r.target->seeks.empty());
  }
  SUBCASE("refused, answered later") {
    Rig r;
    r.target->accept = false;
    r.target->inline_done = false;
    r.turn(+1);
    r.advance(200);
    REQUIRE(r.target->pending);
    r.target->pending(false);  // e.g. from a WinRT thread: posted
    CHECK_NOTHROW(r.loop.drain());
    r.turn(+1);
    r.advance(200);
    CHECK(r.target->seeks.size() == 2);
  }
  SUBCASE("no target at all") {
    Rig r(10, false);
    r.turn(+1);
    CHECK_NOTHROW(r.advance(200));
  }
}

TEST_CASE("seek knob uses its playing and stopped ring colours") {
  Rig r;
  REQUIRE(r.ctx.ring);  // stopped colour from the start
  CHECK(*r.ctx.ring == Rgb{0x3d, 0x1f, 0x00});
  r.target->cb(true);
  r.loop.drain();
  REQUIRE(r.ctx.ring);
  CHECK(*r.ctx.ring == Rgb{23, 65, 30});  // #4cd964 at the new default 30 %
  r.target->cb(false);
  r.loop.drain();
  REQUIRE(r.ctx.ring);
  CHECK(*r.ctx.ring == Rgb{0x3d, 0x1f, 0x00});
}

TEST_CASE("seek knob ring brightness scales its colours") {
  FakeLoop loop;
  FakeContext ctx{loop};
  auto m = make_seek_module([] { return std::unique_ptr<SeekTarget>{}; });
  m->start(ctx, nlohmann::json{{"color", "#c86400"}, {"brightness", 50}, {"idle_color", "#ffffff"}, {"idle_brightness", 0}});
  REQUIRE(ctx.ring);
  CHECK(*ctx.ring == Rgb{0, 0, 0});  // stopped at 0 %
}

TEST_CASE("seek is registered on knobs only, without elements, schema defaults") {
  const ModuleInfo* info = find_module("seek");
  REQUIRE(info);
  CHECK(info->allows(SlotKind::Knob));
  CHECK_FALSE(info->allows(SlotKind::Key));
  CHECK_FALSE(info->allows(SlotKind::StripZone));
  CHECK(info->elements.empty());
  nlohmann::json out;
  CHECK_FALSE(apply_schema(info->settings_schema, nlohmann::json::object(), out).has_value());
  CHECK(out == nlohmann::json{{"step_s", 10}, {"color", "#4cd964"}, {"brightness", 30},
                              {"idle_color", "#3d1f00"}, {"idle_brightness", 100}});
  REQUIRE(info->ring_states.size() == 2);
  CHECK(std::string(info->ring_states[0].brightness_key) == "brightness");
  CHECK(std::string(info->ring_states[1].color_key) == "idle_color");
}

// Reads only: never changes the playback position.
TEST_CASE("manual: gsmtc seek target reads the timeline" * doctest::skip()) {
  auto t = make_gsmtc_seek_target();
  std::atomic<int> calls{0};
  t->start([&](bool playing) {
    ++calls;
    MESSAGE("playing=", playing);
  });
  Sleep(2000);
  CHECK(calls.load() >= 1);
  if (auto tl = t->timeline()) MESSAGE("start=", tl->start_s, " end=", tl->end_s, " pos=", tl->position_s);
  else MESSAGE("no session");
}

TEST_CASE("seek knob bases the next seek on the last target while the player has not caught up") {
  Rig r(10);
  r.turn(+1);
  r.advance(200);
  r.turn(+1);  // the fake timeline still says 100
  r.advance(200);
  REQUIRE(r.target->seeks.size() == 2);
  CHECK(r.target->seeks[1] == doctest::Approx(120));  // start position + 2 x step
}

TEST_CASE("seek knob uses the timeline once the player refreshed it, or after 2 s") {
  SUBCASE("refreshed after the seek") {
    Rig r(10);
    r.turn(+1);
    r.advance(200);
    r.target->tl = SeekTimeline{0, 1000, 500, 0.05};  // updated 50 ms ago, after the seek was sent
    r.turn(+1);
    r.advance(200);
    REQUIRE(r.target->seeks.size() == 2);
    CHECK(r.target->seeks[1] == doctest::Approx(510));
  }
  SUBCASE("stale timeline but 2 s later") {
    Rig r(10);
    r.turn(+1);
    r.advance(200);
    r.target->tl = SeekTimeline{0, 1000, 500, 30.0};  // last updated long before the seek
    r.advance(2500);
    r.turn(+1);
    r.advance(200);
    REQUIRE(r.target->seeks.size() == 2);
    CHECK(r.target->seeks[1] == doctest::Approx(510));
  }
  SUBCASE("stale timeline, playing: the last target advances with time") {
    Rig r(10);
    r.target->cb(true);
    r.loop.drain();
    r.turn(+1);
    r.advance(150);  // seek sent: 110
    r.target->tl = SeekTimeline{0, 1000, 100, 30.0};
    r.advance(850);  // the next flush is 1 s after the seek
    r.turn(+1);
    r.advance(150);
    REQUIRE(r.target->seeks.size() == 2);
    CHECK(r.target->seeks[1] == doctest::Approx(110 + 1.0 + 10));
  }
}

TEST_CASE("seek knob holds detents while a seek is unanswered, then sends them") {
  Rig r(10);
  r.target->inline_done = false;
  r.turn(+1);
  r.advance(200);
  REQUIRE(r.target->seeks.size() == 1);
  auto first = std::exchange(r.target->pending, nullptr);
  r.turn(+1);
  r.turn(+1);
  r.advance(200);
  CHECK(r.target->seeks.size() == 1);  // still in flight
  first(true);
  r.loop.drain();
  REQUIRE(r.target->seeks.size() == 2);
  CHECK(r.target->seeks[1] == doctest::Approx(130));  // 110 + 2 x 10
}

TEST_CASE("seek knob gives up on a seek that is never answered") {
  Rig r(10);
  r.target->inline_done = false;
  r.turn(+1);
  r.advance(200);
  r.turn(+1);
  r.advance(200);
  CHECK(r.target->seeks.size() == 1);
  r.advance(2000);
  CHECK(r.target->seeks.size() == 2);
}

TEST_CASE("seek knob does not re-set the ring on a duplicate playing report") {
  Rig r;
  r.target->cb(true);
  r.loop.drain();
  REQUIRE(r.ctx.ring);
  r.ctx.ring.reset();  // so a second set_ring_color would show
  r.target->cb(true);
  r.loop.drain();
  CHECK_FALSE(r.ctx.ring);
}

TEST_CASE("seek knob destroyed with its timer armed or an answer pending") {
  SUBCASE("timer armed") {
    Rig r;
    r.turn(+1);
    r.m.reset();
    CHECK_NOTHROW(r.advance(500));
  }
  SUBCASE("answer after destruction") {
    Rig r;
    r.target->inline_done = false;
    r.turn(+1);
    r.advance(200);
    auto done = r.target->pending;
    REQUIRE(done);
    r.m.reset();
    done(true);
    CHECK_NOTHROW(r.loop.drain());
  }
}
