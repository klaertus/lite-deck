#include <doctest/doctest.h>
#include <windows.h>
#include <atomic>
#include "fake_context.h"
#include "modules/all_modules.h"
#include "modules/notifications/notifications.h"
#include "modules/schema.h"

using namespace ld;
using namespace std::chrono_literals;

namespace {
struct FakeSource : NotificationSource {
  NotifAccess access = NotifAccess::Allowed;
  std::vector<IncomingNotification> list;
  int fetches = 0;
  std::vector<std::uint32_t> removed;
  std::function<void(std::optional<std::vector<IncomingNotification>>)> pending;  // when hold == true
  bool hold = false;
  bool fail = false;  // fetches answer nullopt (as when access is revoked)
  int checks = 0;
  void request_access(std::function<void(NotifAccess)> done) override { done(access); }
  void check_access(std::function<void(NotifAccess)> done) override {
    ++checks;
    done(access);
  }
  void fetch(std::function<void(std::optional<std::vector<IncomingNotification>>)> done) override {
    ++fetches;
    if (hold) pending = std::move(done);
    else if (fail) done(std::nullopt);
    else done(list);
  }
  void remove(std::uint32_t id) override {
    removed.push_back(id);
    std::erase_if(list, [&](const IncomingNotification& n) { return n.id == id; });
  }
};
struct Rig {
  FakeLoop loop;
  FakeContext ctx{loop};
  FakeSource* src = nullptr;
  std::unique_ptr<Module> m;
  explicit Rig(NotifAccess access = NotifAccess::Allowed, std::vector<IncomingNotification> list = {}) {
    m = make_notifications_module([this, access, list] {
      auto s = std::make_unique<FakeSource>();
      s->access = access;
      s->list = list;
      src = s.get();
      return s;
    });
    nlohmann::json s;
    REQUIRE_FALSE(apply_schema(notifications_module_info().settings_schema, nlohmann::json::object(), s));
    m->start(ctx, s);
    loop.drain();
  }
};
IncomingNotification n(std::uint32_t id, const wchar_t* app, const wchar_t* t, const wchar_t* b) {
  return {id, app, t, b};
}
}  // namespace

TEST_CASE("notifications shows an empty state and polls every second") {
  Rig r;
  Renderer renderer;
  CHECK(render_lit_pixels(*r.m, renderer, 370, 112) > 20);
  CHECK_FALSE(r.m->animating());
  int f = r.src->fetches;
  r.loop.advance(3000ms);
  CHECK(r.src->fetches == f + 3);
}

TEST_CASE("notifications shows the newest and a tap acknowledges it") {
  Rig r(NotifAccess::Allowed, {n(1, L"Discord", L"Alice", L"hi"), n(2, L"Outlook", L"Meeting", L"2 pm")});
  r.loop.advance(1000ms);
  r.m->on_event({EventKind::Tap});
  REQUIRE(r.src->removed.size() == 1);
  CHECK(r.src->removed[0] == 2);  // the newest first-seen
  r.m->on_event({EventKind::Tap});
  CHECK(r.src->removed.back() == 1);
  int inv = r.ctx.invalidations;
  r.m->on_event({EventKind::Tap});  // nothing left: no-op
  CHECK(r.src->removed.size() == 2);
  CHECK(r.ctx.invalidations == inv);
}

TEST_CASE("notifications shows a status when access is denied") {
  Rig r(NotifAccess::Denied);
  Renderer renderer;
  CHECK(render_lit_pixels(*r.m, renderer, 370, 112) > 10);
  int f = r.src->fetches;
  r.loop.advance(5000ms);
  CHECK(r.src->fetches == f);  // no polling when denied
}

TEST_CASE("notifications never overlaps fetches") {
  Rig r;
  r.src->hold = true;
  r.loop.advance(1000ms);
  int f = r.src->fetches;
  r.loop.advance(3000ms);
  CHECK(r.src->fetches == f);  // still waiting for the first answer
  auto done = r.src->pending;
  done(std::vector<IncomingNotification>{});
  r.loop.drain();
  r.loop.advance(1000ms);
  CHECK(r.src->fetches == f + 1);
}

TEST_CASE("notifications recovers from a stuck fetch and ignores the late answer") {
  Rig r;
  r.src->hold = true;
  r.loop.advance(1000ms);
  int f = r.src->fetches;
  auto stuck = r.src->pending;
  r.loop.advance(11000ms);
  CHECK(r.src->fetches > f);
  auto fresh = r.src->pending;
  fresh(std::vector<IncomingNotification>{n(2, L"Outlook", L"Newer", L"b")});
  r.loop.drain();
  stuck(std::vector<IncomingNotification>{n(1, L"Discord", L"Older", L"a")});
  r.loop.drain();
  r.m->on_event({EventKind::Tap});
  REQUIRE(r.src->removed.size() == 1);
  CHECK(r.src->removed[0] == 2);
}

TEST_CASE("notifications status text fits a narrow zone") {
  Rig r(NotifAccess::Denied);
  Renderer renderer;
  CHECK(render_lit_pixels(*r.m, renderer, 200, 112) > 10);
}

TEST_CASE("notifications animates long content") {
  Rig r(NotifAccess::Allowed,
        {n(1, L"Snipping Tool", L"Screenshot saved", L"The screenshot was saved automatically to your Pictures folder")});
  Renderer renderer;
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK(r.m->animating());
}

TEST_CASE("notifications slows its scrolling down after a few passes") {
  Rig r(NotifAccess::Allowed,
        {n(1, L"Snipping Tool", L"Screenshot saved", L"The screenshot was saved automatically to your Pictures folder")});
  Renderer renderer;
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  REQUIRE(r.m->animating());
  CHECK_FALSE(r.m->slow_animation());
  r.loop.advance(120000ms);  // far more than five passes at 60 px/s
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK(r.m->animating());
  CHECK(r.m->slow_animation());
}

TEST_CASE("notifications flashes only for a notification that arrives while running") {
  Rig r(NotifAccess::Allowed, {n(1, L"Discord", L"Alice", L"hi")});
  Renderer renderer;
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK_FALSE(r.m->animating());  // already there at start: no flash
  r.src->list.push_back(n(2, L"Outlook", L"Bob", L"yo"));
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK(r.m->animating());
  CHECK_FALSE(r.m->slow_animation());  // the flash runs at full rate
  r.loop.advance(2000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK_FALSE(r.m->animating());
}

TEST_CASE("notifications flashes once and shows the newest when two arrive in one poll") {
  Rig r(NotifAccess::Allowed, {n(1, L"Discord", L"Alice", L"hi")});
  Renderer renderer;
  r.loop.advance(1000ms);
  r.src->list.push_back(n(2, L"Outlook", L"Bob", L"yo"));
  r.src->list.push_back(n(3, L"Teams", L"Carol", L"hey"));
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK(r.m->animating());
  CHECK_FALSE(r.m->slow_animation());
  r.loop.advance(2000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);
  CHECK_FALSE(r.m->animating());  // a single flash
  // Same picture as a deck that had {1, 2, 3} from the start: the newest of the two is shown.
  Rig ref(NotifAccess::Allowed,
          {n(1, L"Discord", L"Alice", L"hi"), n(2, L"Outlook", L"Bob", L"yo"), n(3, L"Teams", L"Carol", L"hey")});
  ref.loop.advance(1000ms);
  CHECK(render_lit_pixels(*r.m, renderer, 370, 112) == render_lit_pixels(*ref.m, renderer, 370, 112));
}

TEST_CASE("notifications redraws when the age label changes") {
  auto old = n(1, L"Discord", L"Alice", L"hi");
  old.age = 30s;
  Rig r(NotifAccess::Allowed, {old});
  Renderer renderer;
  r.loop.advance(1000ms);
  render_lit_pixels(*r.m, renderer, 370, 112);  // "just now"
  int inv = r.ctx.invalidations;
  r.loop.advance(10000ms);
  CHECK(r.ctx.invalidations == inv);  // still under a minute
  r.loop.advance(25000ms);
  CHECK(r.ctx.invalidations > inv);  // now "1 min"
}

TEST_CASE("notifications is registered on strip zones only") {
  REQUIRE(find_module("notifications"));
  CHECK(find_module("notifications")->allows(SlotKind::StripZone));
}

TEST_CASE("manual: winrt notification source answers" * doctest::skip()) {
  auto s = make_winrt_notification_source();
  std::atomic<int> access{-1};
  s->request_access([&](NotifAccess a) { access = static_cast<int>(a); });
  Sleep(3000);
  MESSAGE("notification access status = " << access.load() << " (0=Allowed 1=Denied 2=Unavailable)");
  CHECK(access.load() >= 0);
}

TEST_CASE("notifications sees access granted later, without polling the list meanwhile") {
  Rig r(NotifAccess::Denied);
  r.loop.advance(300000ms);
  CHECK(r.src->fetches == 0);
  CHECK(r.src->checks == 10);  // one check every 30 s, nothing more
  r.src->access = NotifAccess::Allowed;
  r.src->list = {n(7, L"Discord", L"Alice", L"hi")};
  int inv = r.ctx.invalidations;
  r.loop.advance(30000ms);
  CHECK(r.src->fetches >= 1);
  CHECK(r.ctx.invalidations > inv);
  int checks = r.src->checks;
  r.loop.advance(300000ms);
  CHECK(r.src->checks == checks);  // granted: no more checks while fetches succeed
  r.m->on_event({EventKind::Tap});
  REQUIRE(r.src->removed.size() == 1);
  CHECK(r.src->removed[0] == 7);
}

TEST_CASE("notifications sees access revoked: shows the status and forgets the stale list") {
  Rig r(NotifAccess::Allowed, {n(1, L"Discord", L"Alice", L"hi")});
  r.loop.advance(1000ms);
  r.src->access = NotifAccess::Denied;
  r.src->fail = true;
  r.loop.advance(1000ms);
  CHECK(r.src->checks == 1);
  int f = r.src->fetches;
  r.loop.advance(10000ms);
  CHECK(r.src->fetches == f);  // polling stopped
  r.m->on_event({EventKind::Tap});
  CHECK(r.src->removed.empty());  // the stale notification is gone
  CHECK_FALSE(r.m->animating());
  Renderer renderer;
  CHECK(render_lit_pixels(*r.m, renderer, 370, 112) > 10);  // the access message

  // Granted again: polling resumes and the list comes back.
  r.src->access = NotifAccess::Allowed;
  r.src->fail = false;
  r.loop.advance(30000ms);
  CHECK(r.src->fetches > f);
  r.m->on_event({EventKind::Tap});
  REQUIRE(r.src->removed.size() == 1);
  CHECK(r.src->removed[0] == 1);
}

TEST_CASE("notifications keeps the list on failed fetches while access is still granted, checking rarely") {
  Rig r(NotifAccess::Allowed, {n(1, L"Discord", L"Alice", L"hi")});
  r.loop.advance(1000ms);
  r.src->fail = true;
  r.loop.advance(60000ms);
  CHECK(r.src->checks >= 1);
  CHECK(r.src->checks <= 12);  // at most one check per 5 s of failures, not one per fetch
  r.m->on_event({EventKind::Tap});
  REQUIRE(r.src->removed.size() == 1);  // still shown
  CHECK(r.src->removed[0] == 1);
}
