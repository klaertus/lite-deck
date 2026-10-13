// tests/test_action_golden.cpp: the reference images of the action keys (launcher, hotkey): an
// image icon, a glyph, the title alone, and the "!" of a failed action.
#include <doctest/doctest.h>
#include "action_fakes.h"
#include "golden.h"
#include "module_fixtures.h"

using nlohmann::json;

namespace {

// A launcher key with fake systems, its icon loaded and the launch done.
std::unique_ptr<fixtures::Rig> launcher_rig(const json& settings, std::shared_ptr<fakes::Launcher> launcher,
                                            std::map<std::wstring, std::shared_ptr<const ld::Bitmap>> icons = {}) {
  auto icon_loader = std::make_shared<fakes::IconLoader>();
  icon_loader->icons = std::move(icons);
  auto r = std::make_unique<fixtures::Rig>();
  r->start(ld::launcher_module_info(),
           ld::make_launcher_module(std::move(launcher), ld::ActionDeps{.icons = icon_loader,
                                                                        .icon_runner = fakes::inline_runner(),
                                                                        .runner = fakes::inline_runner()}),
           settings);
  return r;
}

// A 96 x 96 picture: a blue-to-violet disc with a white ring, as an app icon would be.
std::shared_ptr<ld::Bitmap> app_icon() {
  auto b = std::make_shared<ld::Bitmap>();
  b->width = b->height = 96;
  b->bgra.assign(96 * 96 * 4, 0);
  for (int y = 0; y < 96; ++y)
    for (int x = 0; x < 96; ++x) {
      const float d = std::hypot(x + 0.5f - 48.f, y + 0.5f - 48.f);
      const float a = std::clamp(46.f - d, 0.f, 1.f);
      const bool ring = d > 30.f && d < 36.f;
      const float t = static_cast<float>(y) / 95.f;
      const float r = ring ? 255.f : 40.f + 120.f * t, g = ring ? 255.f : 120.f - 60.f * t, bl = ring ? 255.f : 255.f;
      std::uint8_t* p = &b->bgra[(static_cast<std::size_t>(y) * 96 + x) * 4];
      p[0] = static_cast<std::uint8_t>(bl * a);
      p[1] = static_cast<std::uint8_t>(g * a);
      p[2] = static_cast<std::uint8_t>(r * a);
      p[3] = static_cast<std::uint8_t>(255.f * a);
    }
  return b;
}

}  // namespace

TEST_CASE("golden: launcher with an image icon") {
  ld::Renderer r;
  auto rig = launcher_rig({{"path", "C:\\Apps\\Studio.exe"}, {"icon_source", "image"}, {"image", "C:\\Pics\\studio.png"}},
                          std::make_shared<fakes::Launcher>(), {{L"C:\\Pics\\studio.png", app_icon()}});
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "launcher_image");
}

TEST_CASE("golden: launcher with a glyph") {
  ld::Renderer r;
  auto rig = launcher_rig({{"kind", "open"}, {"target", "https://github.com"}, {"icon_source", "glyph"}},
                          std::make_shared<fakes::Launcher>());
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "launcher_glyph");
}

TEST_CASE("golden: hotkey with the title alone") {
  ld::Renderer r;
  auto rig = std::make_unique<fixtures::Rig>();
  rig->start(ld::hotkey_module_info(),
             ld::make_hotkey_module(std::make_shared<fakes::Keyboard>(),
                                    ld::ActionDeps{.icons = std::make_shared<fakes::IconLoader>(),
                                                   .icon_runner = fakes::inline_runner(),
                                                   .runner = fakes::inline_runner()}),
             {{"keys", "Ctrl+Shift+M"}, {"icon_source", "none"}, {"title", "Mute"}, {"label", "Teams"}});
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "hotkey_title");
}

TEST_CASE("golden: launcher after a failed launch") {
  ld::Renderer r;
  auto launcher = std::make_shared<fakes::Launcher>();
  launcher->error = "error 2";
  auto rig = launcher_rig({{"path", "C:\\Apps\\Studio.exe"}, {"icon_source", "glyph"}}, launcher);
  rig->m->on_event(ld::InputEvent{ld::EventKind::Press});
  rig->loop.drain();
  rig->loop.advance(ld::kPressFlash);  // the flash is over, the "!" stays
  auto s = draw(*rig->m, r);
  check_golden(r, *s, "launcher_failed");
}
