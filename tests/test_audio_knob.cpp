#include <doctest/doctest.h>
#include "fake_context.h"
#include "modules/audio/audio_knob.h"
#include "modules/schema.h"

using namespace ld;

namespace {
struct FakeEndpoint : AudioEndpoint {
  float vol = 0.5f;
  bool mute = false;
  std::optional<float> volume() override { return vol; }
  bool set_volume(float v) override { vol = v; return true; }
  std::optional<bool> muted() override { return mute; }
  bool set_muted(bool m) override { mute = m; return true; }
};
struct FakeProvider : AudioEndpointProvider {
  FakeEndpoint* ep;
  int reopens = 0;
  std::function<void(bool)> cb;
  explicit FakeProvider(FakeEndpoint* e) : ep(e) {}
  AudioEndpoint* endpoint() override { return ep; }
  void reopen() override { ++reopens; }
  void watch(std::function<void(bool)> on_change) override { cb = std::move(on_change); }
};
struct Rig {
  FakeLoop loop;
  FakeContext ctx{loop};
  FakeEndpoint ep;
  FakeProvider* provider = nullptr;
  std::unique_ptr<Module> m;
  explicit Rig(Rgb muted_ring = {255, 0, 0}) {
    m = make_audio_knob(AudioFactory{[this] {
      auto p = std::make_unique<FakeProvider>(&ep);
      provider = p.get();
      return p;
    }, muted_ring});
    m->start(ctx, nlohmann::json{{"step", 2}});
  }
};
}  // namespace

TEST_CASE("step_volume clamps and snaps to the step") {
  CHECK(step_volume(0.50f, +1, 2) == doctest::Approx(0.52f));
  CHECK(step_volume(0.513f, +1, 2) == doctest::Approx(0.54f));  // snaps to the nearest step (0.52) first
  CHECK(step_volume(0.99f, +1, 2) == doctest::Approx(1.0f));
  CHECK(step_volume(1.0f, +1, 2) == doctest::Approx(1.0f));
  CHECK(step_volume(0.01f, -1, 2) == doctest::Approx(0.0f));
  CHECK(step_volume(0.0f, -1, 2) == doctest::Approx(0.0f));
}

TEST_CASE("audio knob clamps and snaps to the step") {
  Rig r;
  for (int i = 0; i < 40; ++i) r.m->on_event({EventKind::Rotate, +1});
  CHECK(r.ep.vol == doctest::Approx(1.0f));
  for (int i = 0; i < 60; ++i) r.m->on_event({EventKind::Rotate, -1});
  CHECK(r.ep.vol == doctest::Approx(0.0f));
}

TEST_CASE("audio knob press toggles mute and colors the ring with the state's colour") {
  Rig r(Rgb{255, 0, 0});
  REQUIRE(r.ctx.ring);
  CHECK(*r.ctx.ring == Rgb{0x00, 0x28, 0x3e});  // normal: the module's colour, never the Device one
  r.m->on_event({EventKind::Press});
  CHECK(r.ep.mute);
  REQUIRE(r.ctx.ring);
  CHECK(*r.ctx.ring == Rgb{255, 0, 0});
  r.m->on_event({EventKind::Press});
  CHECK_FALSE(r.ep.mute);
  CHECK(*r.ctx.ring == Rgb{0x00, 0x28, 0x3e});
}

TEST_CASE("a ring state at 0 % brightness is black, not the Device colour") {
  FakeLoop loop;
  FakeContext ctx(loop);
  FakeEndpoint ep;
  auto m = make_audio_knob(AudioFactory{[&ep] { return std::make_unique<FakeProvider>(&ep); }, Rgb{255, 0, 0}});
  m->start(ctx, nlohmann::json{{"step", 2}, {"color", "#ffffff"}, {"brightness", 0}, {"muted_brightness", 50}});
  REQUIRE(ctx.ring);
  CHECK(*ctx.ring == Rgb{0, 0, 0});
  m->on_event({EventKind::Rotate, +1});  // still works
  CHECK(ep.vol == doctest::Approx(0.52f));
  m->on_event({EventKind::Press});
  CHECK(*ctx.ring == Rgb{128, 0, 0});  // muted at 50 %
}

TEST_CASE("volume and mic declare normal and muted ring states with their default colours") {
  for (const char* id : {"volume", "mic"}) {
    const ModuleInfo* info = find_module(id);
    REQUIRE(info);
    REQUIRE(info->ring_states.size() == 2);
    CHECK(std::string(info->ring_states[0].key) == "normal");
    CHECK(std::string(info->ring_states[1].color_key) == "muted_color");
  }
  nlohmann::json v, m;
  REQUIRE_FALSE(apply_schema(find_module("volume")->settings_schema, nlohmann::json::object(), v));
  REQUIRE_FALSE(apply_schema(find_module("mic")->settings_schema, nlohmann::json::object(), m));
  CHECK(v == nlohmann::json{{"step", 2}, {"windows_overlay", true}, {"color", "#00283e"}, {"brightness", 100},
                            {"muted_color", "#3a3a3a"}, {"muted_brightness", 100}});
  CHECK(m["color"] == "#1e0e3e");
  CHECK(m["muted_color"] == "#ff0000");  // red means "muted" only
}

TEST_CASE("audio knob follows external mute changes posted from another thread") {
  Rig r(Rgb{40, 40, 40});
  r.ep.mute = true;      // e.g. muted from the Windows mixer
  r.provider->cb(false);  // callback (normally on a COM thread) -> posted
  r.loop.drain();
  REQUIRE(r.ctx.ring);
  CHECK(*r.ctx.ring == Rgb{40, 40, 40});
}

TEST_CASE("audio knob reopens the endpoint when the default device changes") {
  Rig r;
  r.provider->cb(true);
  r.loop.drain();
  CHECK(r.provider->reopens == 1);
}

TEST_CASE("audio knob without a device ignores input") {
  FakeLoop loop;
  FakeContext ctx(loop);
  struct NoDevice : AudioEndpointProvider {
    AudioEndpoint* endpoint() override { return nullptr; }
    void reopen() override {}
    void watch(std::function<void(bool)>) override {}
  };
  auto m = make_audio_knob(AudioFactory{[] { return std::make_unique<NoDevice>(); }, Rgb{255, 0, 0}});
  m->start(ctx, nlohmann::json{{"step", 2}});
  CHECK_NOTHROW(m->on_event({EventKind::Rotate, +1}));
  CHECK_NOTHROW(m->on_event({EventKind::Press}));
}

TEST_CASE("volume and mic are registered on knobs only") {
  REQUIRE(find_module("volume"));
  REQUIRE(find_module("mic"));
  CHECK(find_module("volume")->allows(SlotKind::Knob));
  CHECK_FALSE(find_module("mic")->allows(SlotKind::Key));
}

TEST_CASE("manual: core audio opens the default render endpoint" * doctest::skip()) {
  auto p = make_core_audio(AudioFlow::Render);
  REQUIRE(p->endpoint());
  CHECK(p->endpoint()->volume().has_value());
}

TEST_CASE("volume knob with the Windows overlay sends volume keys instead of setting the volume") {
  FakeLoop loop;
  FakeContext ctx{loop};
  FakeEndpoint ep;
  std::vector<unsigned short> keys;
  auto m = make_audio_knob(AudioFactory{[&] { return std::make_unique<FakeProvider>(&ep); }, Rgb{40, 40, 40},
                                        [&](unsigned short vk) { keys.push_back(vk); }});
  m->start(ctx, nlohmann::json{{"step", 4}, {"windows_overlay", true}});
  const float before = ep.vol;
  m->on_event({EventKind::Rotate, +1});
  CHECK(keys == std::vector<unsigned short>{0xAF, 0xAF});  // 4 % = two 2 % keys
  m->on_event({EventKind::Rotate, -1});
  CHECK(keys.size() == 4);
  CHECK(keys.back() == 0xAE);
  m->on_event({EventKind::Press});
  CHECK(keys.back() == 0xAD);
  CHECK(ep.vol == doctest::Approx(before));  // never set directly
}

TEST_CASE("volume knob with the Windows overlay sends keys even without an open endpoint") {
  FakeLoop loop;
  FakeContext ctx{loop};
  std::vector<unsigned short> keys;
  auto m = make_audio_knob(AudioFactory{[] { return std::make_unique<FakeProvider>(nullptr); }, Rgb{40, 40, 40},
                                        [&](unsigned short vk) { keys.push_back(vk); }});
  m->start(ctx, nlohmann::json{{"step", 2}, {"windows_overlay", true}});
  m->on_event({EventKind::Rotate, +1});
  m->on_event({EventKind::Press});
  CHECK(keys == std::vector<unsigned short>{0xAF, 0xAD});
}

TEST_CASE("volume knob without the overlay sets the volume directly") {
  FakeLoop loop;
  FakeContext ctx{loop};
  FakeEndpoint ep;
  std::vector<unsigned short> keys;
  auto m = make_audio_knob(AudioFactory{[&] { return std::make_unique<FakeProvider>(&ep); }, Rgb{40, 40, 40},
                                        [&](unsigned short vk) { keys.push_back(vk); }});
  m->start(ctx, nlohmann::json{{"step", 2}, {"windows_overlay", false}});
  m->on_event({EventKind::Rotate, +1});
  CHECK(keys.empty());
}
