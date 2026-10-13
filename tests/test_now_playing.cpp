#include <doctest/doctest.h>
#include <windows.h>
#include <atomic>
#include "fake_context.h"
#include "modules/all_modules.h"
#include "modules/now_playing/now_playing.h"
#include "modules/schema.h"

using namespace ld;
using namespace std::chrono_literals;

namespace {
struct FakeSource : NowPlayingSource {
  std::function<void(std::optional<Track>)> cb;
  void start(std::function<void(std::optional<Track>)> on_change) override { cb = std::move(on_change); }
};
struct Rig {
  FakeLoop loop;
  FakeContext ctx{loop};
  FakeSource* src = nullptr;
  std::unique_ptr<Module> m;
  Rig() {
    m = make_now_playing_module([this] {
      auto s = std::make_unique<FakeSource>();
      src = s.get();
      return s;
    });
    nlohmann::json s;
    REQUIRE_FALSE(apply_schema(now_playing_module_info().settings_schema, nlohmann::json::object(), s));
    m->start(ctx, s);
  }
  void push(std::optional<Track> t) {
    src->cb(std::move(t));
    loop.drain();
  }
};
}  // namespace

TEST_CASE("track_text joins title and artist") {
  CHECK(track_text({L"Shake It", L"Metro Station", true}) == L"Shake It - Metro Station");
  CHECK(track_text({L"Solo", L"", true}) == L"Solo");
}

TEST_CASE("now_playing shows the track and animates only when it overflows") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"Court", L"A", true});
  CHECK(r.ctx.invalidations >= 1);
  CHECK(render_lit_pixels(*r.m, renderer, 430, 112) > 20);
  CHECK_FALSE(r.m->animating());
  r.push(Track{L"A title really far too long to fit in the zone", L"Artist", true});
  render_lit_pixels(*r.m, renderer, 430, 112);  // render computes overflow
  CHECK(r.m->animating());
}

TEST_CASE("now_playing does not scroll a paused track") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"A title really far too long to fit in the zone", L"Artist", false});
  CHECK(render_lit_pixels(*r.m, renderer, 430, 112) > 20);
  CHECK_FALSE(r.m->animating());
  r.push(Track{L"A title really far too long to fit in the zone", L"Artist", true});
  render_lit_pixels(*r.m, renderer, 430, 112);
  CHECK(r.m->animating());
}

TEST_CASE("now_playing progress bar advances once per second while playing only") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"A", L"B", true, 0, 60});  // 60 s track: the ~370 px bar gains ~6 px per second
  render_lit_pixels(*r.m, renderer, 430, 112);
  int inv = r.ctx.invalidations;
  r.loop.advance(1000ms);
  CHECK(r.ctx.invalidations == inv + 1);
  render_lit_pixels(*r.m, renderer, 430, 112);
  r.push(Track{L"A", L"B", false, 1, 60});
  render_lit_pixels(*r.m, renderer, 430, 112);
  inv = r.ctx.invalidations;
  r.loop.advance(3000ms);
  CHECK(r.ctx.invalidations == inv);  // paused: the bar is still
  CHECK_FALSE(r.m->animating());
}

TEST_CASE("now_playing without a timeline never ticks") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"A", L"B", true});
  render_lit_pixels(*r.m, renderer, 430, 112);
  int inv = r.ctx.invalidations;
  r.loop.advance(5000ms);
  CHECK(r.ctx.invalidations == inv);
}

TEST_CASE("now_playing clears the title when the session ends") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"A title really far too long to fit in the zone", L"Artist", true});
  render_lit_pixels(*r.m, renderer, 430, 112);
  REQUIRE(r.m->animating());
  int inv = r.ctx.invalidations;
  r.push(std::nullopt);
  CHECK(r.ctx.invalidations == inv + 1);
  render_lit_pixels(*r.m, renderer, 430, 112);
  CHECK_FALSE(r.m->animating());
  CHECK(render_lit_pixels(*r.m, renderer, 430, 112) > 5);  // "Nothing playing"
}

TEST_CASE("now_playing ignores touches and duplicate updates") {
  Rig r;
  r.push(Track{L"A", L"B", true});
  int inv = r.ctx.invalidations;
  r.push(Track{L"A", L"B", true});
  CHECK(r.ctx.invalidations == inv);
  CHECK_NOTHROW(r.m->on_event({EventKind::Tap}));
}

TEST_CASE("now_playing clamps a position past the duration") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"A", L"B", true, 500, 60});
  CHECK_NOTHROW(render_lit_pixels(*r.m, renderer, 430, 112));
  r.loop.advance(3000ms);
  CHECK_NOTHROW(render_lit_pixels(*r.m, renderer, 430, 112));
  // The bar is full: as many lit pixels as at the very end of the track.
  Rig end;
  end.push(Track{L"A", L"B", true, 60, 60});
  CHECK(render_lit_pixels(*r.m, renderer, 430, 112) == render_lit_pixels(*end.m, renderer, 430, 112));
}

TEST_CASE("now_playing treats a near-identical report as a duplicate") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"A", L"B", true, 10, 60});
  render_lit_pixels(*r.m, renderer, 430, 112);
  r.loop.advance(2000ms);
  int inv = r.ctx.invalidations;
  r.push(Track{L"A", L"B", true, 12.2, 60});  // 0.2 s off the extrapolated 12 s
  CHECK(r.ctx.invalidations == inv);
  r.push(Track{L"A", L"B", true, 30, 60});  // a seek
  CHECK(r.ctx.invalidations == inv + 1);
  r.push(Track{L"A", L"B", false, 30, 60});  // paused
  CHECK(r.ctx.invalidations == inv + 2);
}

TEST_CASE("now_playing without text does not redraw every second") {
  Rig r;
  Renderer renderer;
  r.push(Track{L"", L"", true, 0, 60});
  render_lit_pixels(*r.m, renderer, 430, 112);
  int inv = r.ctx.invalidations;
  r.loop.advance(5000ms);
  CHECK(r.ctx.invalidations == inv);
}

TEST_CASE("now_playing is registered on strip zones only") {
  REQUIRE(find_module("now_playing"));
  CHECK(find_module("now_playing")->allows(SlotKind::StripZone));
  CHECK_FALSE(find_module("now_playing")->allows(SlotKind::Key));
}

TEST_CASE("manual: gsmtc source reports something" * doctest::skip()) {
  auto src = make_gsmtc_source();
  std::atomic<int> calls{0};
  src->start([&](std::optional<Track> t) {
    ++calls;
    if (t) { char buf[512]; WideCharToMultiByte(CP_UTF8, 0, t->title.c_str(), -1, buf, sizeof buf, nullptr, nullptr); MESSAGE("track: ", buf, " playing=", t->playing); }
    else MESSAGE("no session");
  });
  Sleep(2000);
  CHECK(calls.load() >= 1);
}

namespace {
std::vector<unsigned> pixels(Module& m, Renderer& r) {
  Surface s(r, 430, 112);
  {
    Canvas c = s.canvas();
    c.clear(Rgb{0, 0, 0});
    m.render(c);
  }
  std::vector<unsigned> v;
  for (int y = 0; y < 112; ++y)
    for (int x = 0; x < 430; ++x) {
      auto p = s.pixel(x, y);
      v.push_back(p.r << 16 | p.g << 8 | p.b);
    }
  return v;
}
}  // namespace

TEST_CASE("now_playing pausing keeps the scroll position") {
  const std::wstring title = L"A title really far too long to fit in the zone";
  Renderer renderer;
  Rig a;  // plays for 1 s, then pauses
  a.push(Track{title, L"Artist", true});
  a.loop.advance(1000ms);
  a.push(Track{title, L"Artist", false});
  Rig b;  // paused from the start, same elapsed time
  b.push(Track{title, L"Artist", false});
  b.loop.advance(1000ms);
  CHECK(pixels(*a.m, renderer) == pixels(*b.m, renderer));
}

TEST_CASE("now_playing slows its scrolling down after a few passes, and a new track restarts it") {
  const std::wstring title = L"A title really far too long to fit in the zone";
  Rig r;
  Renderer renderer;
  r.push(Track{title, L"Artist", true});
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 430, 112);
  REQUIRE(r.m->animating());
  CHECK_FALSE(r.m->slow_animation());
  r.loop.advance(120000ms);  // far more than five passes
  render_lit_pixels(*r.m, renderer, 430, 112);
  CHECK(r.m->animating());
  CHECK(r.m->slow_animation());
  r.push(Track{title + L" (live)", L"Artist", true});  // new track: smooth again
  render_lit_pixels(*r.m, renderer, 430, 112);
  CHECK_FALSE(r.m->slow_animation());
}
