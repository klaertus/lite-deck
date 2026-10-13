#include <doctest/doctest.h>
#include <windows.h>
#include <vector>
#include "fake_context.h"
#include "modules/all_modules.h"
#include "modules/media/media.h"

TEST_CASE("media knob maps rotation and press to media keys") {
  FakeLoop loop;
  FakeContext ctx(loop);
  std::vector<unsigned short> sent;
  auto m = ld::make_media_module([&](unsigned short vk) { sent.push_back(vk); });
  m->start(ctx, nlohmann::json::object());
  m->on_event({ld::EventKind::Rotate, +1});
  m->on_event({ld::EventKind::Rotate, -1});
  m->on_event({ld::EventKind::Press});
  m->on_event({ld::EventKind::Tap});  // ignored
  CHECK(sent == std::vector<unsigned short>{VK_MEDIA_NEXT_TRACK, VK_MEDIA_PREV_TRACK, VK_MEDIA_PLAY_PAUSE});
}

TEST_CASE("media skips one track per notch when a turn reports several; none for 0") {
  FakeLoop loop;
  FakeContext ctx(loop);
  std::vector<unsigned short> sent;
  auto m = ld::make_media_module([&](unsigned short vk) { sent.push_back(vk); });
  m->start(ctx, nlohmann::json::object());
  m->on_event({ld::EventKind::Rotate, +3});
  m->on_event({ld::EventKind::Rotate, 0});
  m->on_event({ld::EventKind::Rotate, -2});
  CHECK(sent == std::vector<unsigned short>{VK_MEDIA_NEXT_TRACK, VK_MEDIA_NEXT_TRACK, VK_MEDIA_NEXT_TRACK,
                                            VK_MEDIA_PREV_TRACK, VK_MEDIA_PREV_TRACK});
}

TEST_CASE("media is registered on knobs") {
  REQUIRE(ld::find_module("media"));
  CHECK(ld::find_module("media")->allows(ld::SlotKind::Knob));
}

TEST_CASE("media sets its normal ring colour at start") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = ld::make_media_module([](unsigned short) {});
  m->start(ctx, nlohmann::json{{"color", "#0c3a14"}, {"brightness", 50}});
  REQUIRE(ctx.ring);
  CHECK(*ctx.ring == ld::Rgb{6, 29, 10});
  REQUIRE(ld::find_module("media")->ring_states.size() == 1);
}
