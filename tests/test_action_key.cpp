// tests/test_action_key.cpp: the shared base of the launcher and hotkey keys: icon sources, the
// icon cache, the glyphs, the press flash and the "!" of a failure, and the engine's image icons.
#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "action_fakes.h"
#include "core/log.h"
#include "golden.h"
#include "module_fixtures.h"
#include "modules/common/action_key.h"

using nlohmann::json;

namespace {

struct Started {
  std::unique_ptr<fixtures::Rig> rig = std::make_unique<fixtures::Rig>();
  std::shared_ptr<fakes::IconLoader> icons = std::make_shared<fakes::IconLoader>();
  std::shared_ptr<fakes::Launcher> launcher = std::make_shared<fakes::Launcher>();
  fixtures::Rig& operator*() { return *rig; }
  std::optional<ld::KeyElement> element(std::string_view key) { return fixtures::element_of(*rig->m, key); }
  ld::ActionKey& key() { return dynamic_cast<ld::ActionKey&>(*rig->m); }
};

// A launcher key with fake systems; the icon worker runs inline.
Started launcher_key(const json& settings, std::map<std::wstring, std::shared_ptr<const ld::Bitmap>> icons = {}) {
  Started s;
  s.icons->icons = std::move(icons);
  s.rig->start(ld::launcher_module_info(),
               ld::make_launcher_module(s.launcher, ld::ActionDeps{.icons = s.icons,
                                                                   .icon_runner = fakes::inline_runner(),
                                                                   .runner = fakes::inline_runner()}),
               settings);
  return s;
}

}  // namespace

TEST_CASE("parse_glyph: an emoji, a code, and an icon font's glyph") {
  CHECK(ld::parse_glyph("\xF0\x9F\x9A\x80").text == L"\U0001F680");  // 🚀 typed
  CHECK(ld::parse_glyph("\xF0\x9F\x9A\x80").font.empty());
  CHECK(ld::parse_glyph(" U+1F680 ").text == L"\U0001F680");
  CHECK(ld::parse_glyph("\\U0001f680").text == L"\U0001F680");
  const ld::Glyph fluent = ld::parse_glyph("u+E713");
  CHECK(fluent.text == L"\uE713");
  CHECK((fluent.font == L"Segoe Fluent Icons" || fluent.font == L"Segoe MDL2 Assets"));
  CHECK(ld::parse_glyph("\xEE\x9C\x93").font == fluent.font);  // U+E713 typed as the character
  CHECK(ld::parse_glyph("A").text == L"A");
  CHECK(ld::parse_glyph("A").font.empty());
  CHECK(ld::parse_glyph("U+XYZ").text == L"U+XYZ");      // not a code: the text itself
  CHECK(ld::parse_glyph("U+D800").text == L"U+D800");    // a lone surrogate is no code point
  CHECK(ld::parse_glyph("U+110000").text == L"U+110000");
}

TEST_CASE("is_url, name_for_log and expand_env") {
  CHECK(ld::is_url(L"https://example.com/x"));
  CHECK(ld::is_url(L"mailto:me@example.com"));
  CHECK(ld::is_url(L"ms-settings:display"));
  CHECK_FALSE(ld::is_url(L"C:\\Windows\\notepad.exe"));
  CHECK_FALSE(ld::is_url(L"notepad.exe"));
  CHECK_FALSE(ld::is_url(L"1x:y"));
  CHECK(ld::name_for_log(L"C:\\Apps\\Tool\\tool.exe") == "tool.exe");
  CHECK(ld::name_for_log(L"D:\\Music\\") == "Music");
  CHECK(ld::name_for_log(L"https://user:pw@example.com/secret?q=1") == "a https link");
  REQUIRE(SetEnvironmentVariableW(L"LD_ACTION_TEST", L"D:\\Tools"));
  CHECK(ld::expand_env(L"%LD_ACTION_TEST%\\app.exe") == L"D:\\Tools\\app.exe");
  CHECK(ld::expand_env(L"%LD_NO_SUCH_VARIABLE%\\x") == L"%LD_NO_SUCH_VARIABLE%\\x");
  CHECK(ld::expand_env(L"plain") == L"plain");
  SetEnvironmentVariableW(L"LD_ACTION_TEST", nullptr);
}

TEST_CASE("IconCache keeps at most its count and its bytes, the least recently used going first") {
  ld::IconCache cache(64, 1u << 30);
  auto icon = fakes::disc(8, 8, ld::Rgb{255, 0, 0});  // 256 bytes
  for (int i = 0; i < 70; ++i) cache.put(std::to_wstring(i), icon);
  CHECK(cache.size() == 64);
  CHECK(cache.bytes() == 64 * 256);
  CHECK_FALSE(cache.get(L"0"));  // the six oldest are gone
  CHECK_FALSE(cache.get(L"5"));
  CHECK(cache.get(L"6"));
  cache.put(L"70", icon);  // "6" was just used: "7" goes
  CHECK(cache.get(L"6"));
  CHECK_FALSE(cache.get(L"7"));

  ld::IconCache tight(64, 1000);
  tight.put(L"a", icon);
  tight.put(L"b", icon);
  tight.put(L"c", icon);
  CHECK(tight.size() == 3);
  tight.put(L"d", icon);  // 4 x 256 > 1000
  CHECK(tight.size() == 3);
  CHECK_FALSE(tight.get(L"a"));
  tight.put(L"huge", fakes::disc(32, 32, ld::Rgb{0, 0, 255}));  // 4096 bytes: never kept
  CHECK_FALSE(tight.get(L"huge"));
  CHECK(tight.bytes() <= 1000);
  tight.put(L"b", fakes::disc(4, 4, ld::Rgb{0, 255, 0}));  // replaced: its bytes are counted once
  CHECK(tight.bytes() == 2 * 256 + 64);
}

TEST_CASE("action key: the program's icon, loaded off the loop thread") {
  auto icon = fakes::disc(96, 96, ld::Rgb{0, 160, 255});
  auto s = launcher_key({{"path", "C:\\Apps\\Studio.exe"}}, {{L"C:\\Apps\\Studio.exe", icon}});
  REQUIRE(s.icons->asked.size() == 1);
  CHECK(s.icons->asked[0] == ld::IconRequest{.source = ld::IconSource::Program, .path = L"C:\\Apps\\Studio.exe"});
  auto e = s.element("icon");
  REQUIRE(e);
  CHECK(e->image == icon);
  CHECK(e->text.empty());
  CHECK(s.element("label")->text == L"Studio");
  CHECK_FALSE(s.element("title"));
}

TEST_CASE("action key: the icon arrives later; until then nothing is drawn in its place") {
  fakes::HeldRunner held;
  auto icons = std::make_shared<fakes::IconLoader>();
  icons->icons[L"C:\\a.exe"] = fakes::disc(16, 16, ld::Rgb{255, 255, 0});
  fixtures::Rig rig;
  rig.start(ld::launcher_module_info(),
            ld::make_launcher_module(std::make_shared<fakes::Launcher>(),
                                     {.icons = icons, .icon_runner = held.runner(), .runner = fakes::inline_runner()}),
            {{"path", "C:\\a.exe"}});
  CHECK(icons->asked.empty());  // not loaded on the loop thread
  auto e = fixtures::element_of(*rig.m, "icon");
  CHECK((!e || (e->text.empty() && !e->image)));
  const int before = rig.ctx.invalidations;
  held.run_all();
  rig.loop.drain();
  CHECK(fixtures::element_of(*rig.m, "icon")->image);
  CHECK(rig.ctx.invalidations == before + 1);
}

TEST_CASE("action key: an image file, its path expanded") {
  REQUIRE(SetEnvironmentVariableW(L"LD_ACTION_PICS", L"D:\\Pics"));
  auto icon = fakes::disc(96, 48, ld::Rgb{255, 80, 0});
  auto s = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "image"}, {"image", "%LD_ACTION_PICS%\\x.png"}},
                        {{L"D:\\Pics\\x.png", icon}});
  SetEnvironmentVariableW(L"LD_ACTION_PICS", nullptr);
  REQUIRE(s.icons->asked.size() == 1);
  CHECK(s.icons->asked[0].source == ld::IconSource::Image);
  CHECK(s.icons->asked[0].path == L"D:\\Pics\\x.png");
  CHECK(s.element("icon")->image == icon);
}

TEST_CASE("action key: a glyph in its colour, an icon font's glyph in that font") {
  auto s = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "glyph"}, {"glyph", "U+E713"}, {"glyph_color", "#12ab34"}});
  CHECK(s.icons->asked.empty());
  auto e = s.element("icon");
  REQUIRE(e);
  CHECK(e->text == L"\uE713");
  CHECK_FALSE(e->font.empty());
  CHECK(e->color == ld::Rgb{0x12, 0xab, 0x34});
  auto emoji = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "glyph"}});
  CHECK(emoji.element("icon")->text == L"\U0001F680");  // the default glyph
  CHECK(emoji.element("icon")->font.empty());
}

TEST_CASE("action key: none shows the title alone; the label only when both are set") {
  auto plain = launcher_key({{"path", "C:\\Tools\\Studio.exe"}, {"icon_source", "none"}});
  CHECK_FALSE(plain.element("icon"));
  CHECK(plain.element("title")->text == L"Studio");
  CHECK_FALSE(plain.element("label"));
  auto labelled = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "none"}, {"label", "Mail"}});
  CHECK(labelled.element("title")->text == L"Mail");
  CHECK_FALSE(labelled.element("label"));
  auto both = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "none"}, {"title", "Mute"}, {"label", "Teams"}});
  CHECK(both.element("title")->text == L"Mute");
  CHECK(both.element("label")->text == L"Teams");
}

TEST_CASE("action key: a missing icon shows a generic one, and the log says so once, without the path") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_action_key_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    auto program = launcher_key({{"path", "C:\\Secret\\gone.exe"}});
    CHECK(program.element("icon")->text == L"\U0001F4C4");  // 📄
    auto image = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "image"}, {"image", "C:\\Secret\\x.png"}});
    CHECK(image.element("icon")->text == L"\U0001F5BC\uFE0F");  // 🖼️
    auto url = launcher_key({{"kind", "open"}, {"target", "https://example.com/private?token=1"}});
    CHECK(url.element("icon")->text == L"\U0001F310");  // 🌐
    CHECK(url.element("label")->text == L"example.com");
    auto empty_image = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "image"}});
    CHECK(empty_image.icons->asked.empty());
    CHECK(empty_image.element("icon")->text == L"\U0001F5BC\uFE0F");
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("no icon for gone.exe") != std::string::npos);
  CHECK(log.find("no icon for a https link") != std::string::npos);
  CHECK(log.find("Secret") == std::string::npos);
  CHECK(log.find("token") == std::string::npos);
  std::size_t lines = 0;
  for (std::size_t at = log.find("no icon for gone.exe"); at != std::string::npos; at = log.find("no icon for gone.exe", at + 1))
    ++lines;
  CHECK(lines == 1);
  fs::remove_all(dir);
}

TEST_CASE("action key: a press lights the key up for 150 ms; a failure shows the \"!\" for 1.5 s") {
  auto s = launcher_key({{"path", "C:\\x.exe"}, {"icon_source", "glyph"}});
  ld::Renderer r;
  auto base = draw(*s.rig->m, r);
  s.launcher->error = "error 2: The system cannot find the file specified";
  s.rig->m->on_event(ld::InputEvent{ld::EventKind::Press});
  CHECK(s.key().flashing());
  s.rig->loop.drain();  // the launch failed
  CHECK(s.key().failing());
  auto lit = draw(*s.rig->m, r);
  CHECK(lit->pixel(5, 100).r > 50);  // the black corner turned grey
  CHECK(lit->pixel(5, 100).r == lit->pixel(5, 100).b);
  CHECK(base->pixel(5, 100).r == 0);
  CHECK(saturated(*lit, 80, 0, 112, 30) > 20);  // the red "!" in the top-right corner
  CHECK(saturated(*base, 80, 0, 112, 30) == 0);
  s.rig->loop.advance(ld::kPressFlash);
  CHECK_FALSE(s.key().flashing());
  CHECK(s.key().failing());
  s.rig->loop.advance(ld::kFailureShown);
  CHECK_FALSE(s.key().failing());
  auto after = draw(*s.rig->m, r);
  CHECK(same_rows(*after, *base, 0, 112));
}

TEST_CASE("key engine: an image icon is drawn at its size, centred in its row, and shrinks to fit") {
  static constexpr ld::ElementSpec kSpecs[] = {
      {.key = "icon", .label = "Icon", .size = 40, .weight = 400, .min_size = 10, .max_size = 80, .icon = true,
       .kind = ld::ElementKind::Icon, .row = ld::Row::Middle},
  };
  ld::Renderer r;
  ld::Surface s(r, 112, 112);
  ld::Canvas c = s.canvas();
  c.clear(ld::Rgb{0, 0, 0});
  std::vector<ld::KeyElement> content{ld::KeyElement{.key = "icon", .image = fakes::disc(96, 48, ld::Rgb{0, 255, 0})}};
  auto items = ld::layout_key(c, content, kSpecs);
  REQUIRE(items.size() == 1);
  CHECK(items[0].text.size == 40.f);  // 80 x 40: fits in the 100 px row
  ld::render_key(c, content, kSpecs);
  content[0].size = 60.f;  // 120 x 60: wider than the row, shrunk to 100 x 50
  items = ld::layout_key(c, content, kSpecs);
  CHECK(items[0].text.size == doctest::Approx(50.f));
  content[0].image = std::make_shared<ld::Bitmap>();  // an empty picture: nothing to show
  CHECK(ld::layout_key(c, content, kSpecs).empty());
  c.clear(ld::Rgb{0, 0, 0});
  content[0] = ld::KeyElement{.key = "icon", .image = fakes::disc(96, 48, ld::Rgb{0, 255, 0})};
  ld::render_key(c, content, kSpecs);
  {
    ld::Canvas moved = std::move(c);
  }
  const Ink in = ink(s);
  CHECK(in.left >= 15);  // 80 px wide, centred in 6..106
  CHECK(in.right <= 96);
  CHECK(in.top >= 29);   // 40 px high, centred in 24..76
  CHECK(in.bottom <= 71);
  CHECK(s.pixel(56, 50).g > 200);
}

// Real shell icons and a real picture, drawn on keys saved to %TEMP%\ld_action_icons\*.jpg to look
// at. Needs the machine's shell: run it by hand with --no-skip.
TEST_CASE("manual: action key icons from the system" * doctest::skip()) {
  namespace fs = std::filesystem;
  const fs::path out = fs::temp_directory_path() / "ld_action_icons";
  fs::create_directories(out);
  const std::string golden = (fs::path(LD_TEST_DIR) / "golden" / "weather_sunny.png").string();
  const std::vector<std::pair<const char*, json>> keys = {
      {"notepad", {{"path", "%SystemRoot%\\System32\\notepad.exe"}}},
      {"bare", {{"path", "notepad.exe"}}},
      {"folder", {{"kind", "open"}, {"target", "%SystemRoot%"}}},
      {"browser", {{"kind", "open"}, {"target", "https://example.com"}}},
      {"cmd", {{"kind", "command"}, {"command_line", "echo"}}},
      {"picture", {{"path", "x.exe"}, {"icon_source", "image"}, {"image", golden}}},
  };
  ld::Renderer r;
  for (const auto& [name, settings] : keys) {
    INFO(name);
    fixtures::Rig rig;
    rig.start(ld::launcher_module_info(),
              ld::make_launcher_module(std::make_shared<fakes::Launcher>(),
                                       {.icon_runner = fakes::inline_runner(), .runner = fakes::inline_runner()}),
              settings);
    auto e = fixtures::element_of(*rig.m, "icon");
    REQUIRE(e);
    CHECK(e->image);
    auto s = draw(*rig.m, r);
    const std::vector<std::uint8_t> jpg = s->encode_jpeg(95, false);
    std::ofstream(out / (std::string(name) + ".jpg"), std::ios::binary)
        .write(reinterpret_cast<const char*>(jpg.data()), static_cast<std::streamsize>(jpg.size()));
  }
  MESSAGE("keys written to " << out.string());
}

TEST_CASE("action key: an icon still loading after 2 s shows the generic one until it arrives") {
  fakes::HeldRunner held;
  auto icons = std::make_shared<fakes::IconLoader>();
  icons->icons[L"\\\\nas\\apps\\a.exe"] = fakes::disc(16, 16, ld::Rgb{255, 255, 0});
  fixtures::Rig rig;
  rig.start(ld::launcher_module_info(),
            ld::make_launcher_module(std::make_shared<fakes::Launcher>(),
                                     {.icons = icons, .icon_runner = held.runner(), .runner = fakes::inline_runner()}),
            {{"path", "\\\\nas\\apps\\a.exe"}});
  CHECK_FALSE(fixtures::element_of(*rig.m, "icon")->text.size());
  rig.loop.advance(ld::kIconPatience);
  CHECK(fixtures::element_of(*rig.m, "icon")->text == L"\U0001F4C4");
  held.run_all();
  rig.loop.drain();
  auto e = fixtures::element_of(*rig.m, "icon");
  CHECK(e->image);
  CHECK(e->text.empty());
}
