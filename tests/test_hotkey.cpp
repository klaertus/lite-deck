// tests/test_hotkey.cpp: the key grammar, the exact keyboard events, and the hotkey key with a fake
// keyboard. No keystroke is ever sent.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "action_fakes.h"
#include "core/config.h"
#include "core/log.h"
#include "module_fixtures.h"
#include "modules/hotkey/hotkey.h"

using nlohmann::json;

namespace {

ld::KeyCode vk(std::uint16_t v, bool extended = false) { return ld::KeyCode{.vk = v, .extended = extended}; }

// One expected event: the key, down or up, extended or not (scan codes are vk + 0x100 in the fake).
struct Ev {
  std::uint16_t vk;
  bool up;
  bool extended = false;
};

void check_events(const std::vector<INPUT>& in, const std::vector<Ev>& want) {
  REQUIRE(in.size() == want.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    INFO("event " << i);
    CHECK(in[i].type == INPUT_KEYBOARD);
    CHECK(in[i].ki.wVk == want[i].vk);
    CHECK(in[i].ki.wScan == want[i].vk + 0x100);
    CHECK(((in[i].ki.dwFlags & KEYEVENTF_KEYUP) != 0) == want[i].up);
    CHECK(((in[i].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0) == want[i].extended);
    CHECK((in[i].ki.dwFlags & KEYEVENTF_UNICODE) == 0);
  }
}

struct Key {
  std::unique_ptr<fixtures::Rig> rig = std::make_unique<fixtures::Rig>();
  std::shared_ptr<fakes::Keyboard> keyboard = std::make_shared<fakes::Keyboard>();
  fakes::HeldRunner held;
  void press() { rig->m->on_event(ld::InputEvent{ld::EventKind::Press}); }
  void finish() {
    held.run_all();
    rig->loop.drain();
  }
  ld::ActionKey& key() { return dynamic_cast<ld::ActionKey&>(*rig->m); }
};

Key start(const json& settings) {
  Key k;
  k.rig->start(ld::hotkey_module_info(),
               ld::make_hotkey_module(k.keyboard, ld::ActionDeps{.icons = std::make_shared<fakes::IconLoader>(),
                                                                 .icon_runner = fakes::inline_runner(),
                                                                 .runner = k.held.runner()}),
               settings);
  return k;
}

}  // namespace

TEST_CASE("parse_keys: modifiers and a key, case-insensitive, in sequences") {
  ld::KeyParse p = ld::parse_keys("Ctrl+Shift+M");
  CHECK(p.error.empty());
  REQUIRE(p.combos.size() == 1);
  CHECK(p.combos[0] == ld::KeyCombo{{vk(VK_CONTROL), vk(VK_SHIFT)}, vk('M')});
  CHECK(p.display == "Ctrl+Shift+M");
  CHECK(ld::parse_keys(" ctrl + SHIFT + m ").combos == p.combos);
  CHECK(ld::parse_keys(" ctrl + SHIFT + m ").display == "Ctrl+Shift+M");

  p = ld::parse_keys("Win+Shift+S, Enter");
  REQUIRE(p.combos.size() == 2);
  CHECK(p.combos[0] == ld::KeyCombo{{vk(VK_LWIN, true), vk(VK_SHIFT)}, vk('S')});
  CHECK(p.combos[1] == ld::KeyCombo{{}, vk(VK_RETURN)});
  CHECK(p.display == "Win+Shift+S, Enter");

  CHECK(ld::parse_keys("Win").combos[0] == ld::KeyCombo{{}, vk(VK_LWIN, true)});  // a modifier alone
  CHECK(ld::parse_keys("Ctrl+Alt").combos[0] == ld::KeyCombo{{vk(VK_CONTROL)}, vk(VK_MENU)});
  CHECK(ld::parse_keys("F1").combos[0].key == vk(VK_F1));
  CHECK(ld::parse_keys("f24").combos[0].key == vk(VK_F24));
  CHECK(ld::parse_keys("7").combos[0].key == vk('7'));
  CHECK(ld::parse_keys("Numpad7").combos[0].key == vk(VK_NUMPAD7));
  CHECK(ld::parse_keys("NumpadEnter").combos[0].key == vk(VK_RETURN, true));
  CHECK(ld::parse_keys("NumpadDivide").combos[0].key == vk(VK_DIVIDE, true));
  CHECK(ld::parse_keys("NumpadAdd").combos[0].key == vk(VK_ADD));
  CHECK(ld::parse_keys("escape").combos[0].key == vk(VK_ESCAPE));
  CHECK(ld::parse_keys("PageDown").combos[0].key == vk(VK_NEXT, true));
  CHECK(ld::parse_keys("Alt+Left").combos[0].key == vk(VK_LEFT, true));
  CHECK(ld::parse_keys("PrintScreen").combos[0].key == vk(VK_SNAPSHOT, true));
  CHECK(ld::parse_keys("Pause").combos[0].key == vk(VK_PAUSE));
  CHECK(ld::parse_keys("CapsLock").combos[0].key == vk(VK_CAPITAL));
  CHECK(ld::parse_keys("MediaPlayPause").combos[0].key == vk(VK_MEDIA_PLAY_PAUSE, true));
  CHECK(ld::parse_keys("MediaPrev").combos[0].key == vk(VK_MEDIA_PREV_TRACK, true));
  CHECK(ld::parse_keys("VolumeMute").combos[0].key == vk(VK_VOLUME_MUTE, true));
  CHECK(ld::parse_keys("Ctrl+Minus").combos[0].key == ld::KeyCode{.ch = L'-'});
  CHECK(ld::parse_keys("Ctrl+Plus").combos[0].key == ld::KeyCode{.ch = L'+'});
  CHECK(ld::parse_keys("Ctrl+Comma").combos[0].key == ld::KeyCode{.ch = L','});
  CHECK(ld::parse_keys("Ctrl+/").combos[0].key == ld::KeyCode{.ch = L'/'});
  CHECK(ld::parse_keys("Ctrl+.").combos[0].key == ld::KeyCode{.ch = L'.'});
  CHECK(ld::parse_keys("Ctrl+Period").display == "Ctrl+Period");
  CHECK(ld::parse_keys("A, B, C, D, E, F, G, H").combos.size() == 8);  // the most
  ld::KeyParse empty = ld::parse_keys("   ");
  CHECK(empty.error.empty());
  CHECK(empty.combos.empty());
}

TEST_CASE("parse_keys: a precise message for each mistake") {
  CHECK(ld::parse_keys("Ctrlx").error == "keys: unknown key \"Ctrlx\"");
  CHECK(ld::parse_keys("Ctrl+Shift+Mm").error == "keys: unknown key \"Mm\"");
  CHECK(ld::parse_keys("F25").error == "keys: unknown key \"F25\"");
  CHECK(ld::parse_keys("F0").error == "keys: unknown key \"F0\"");
  CHECK(ld::parse_keys("Numpad10").error == "keys: unknown key \"Numpad10\"");
  CHECK(ld::parse_keys("Ctrl+").error == "keys: a missing key in \"Ctrl+\" (write Plus for the + key)");
  CHECK(ld::parse_keys("A+B").error == "keys: two keys in \"A+B\"");
  CHECK(ld::parse_keys("A+Ctrl").error == "keys: the modifiers come before the key in \"A+Ctrl\"");
  CHECK(ld::parse_keys("Ctrl+control+A").error == "keys: \"Ctrl\" twice in \"Ctrl+control+A\"");
  CHECK(ld::parse_keys("A,,B").error == "keys: an empty combination (two commas, or a comma at an end)");
  CHECK(ld::parse_keys("A,").error == "keys: an empty combination (two commas, or a comma at an end)");
  CHECK(ld::parse_keys("A,B,C,D,E,F,G,H,I").error == "keys: at most 8 combinations");
  ld::KeyParse bad = ld::parse_keys("Ctrl+A, Nope");
  CHECK(bad.combos.empty());  // nothing half-parsed
  CHECK(bad.display.empty());
}

TEST_CASE("combo_inputs: modifiers down, the key, then the modifiers up in reverse order") {
  fakes::Keyboard kb;
  check_events(ld::combo_inputs(ld::parse_keys("Ctrl+Shift+M").combos[0], kb),
               {{VK_CONTROL, false}, {VK_SHIFT, false}, {'M', false}, {'M', true}, {VK_SHIFT, true}, {VK_CONTROL, true}});
  check_events(ld::combo_inputs(ld::parse_keys("Win+Left").combos[0], kb),
               {{VK_LWIN, false, true}, {VK_LEFT, false, true}, {VK_LEFT, true, true}, {VK_LWIN, true, true}});
  check_events(ld::combo_inputs(ld::parse_keys("Ctrl+Alt+Delete").combos[0], kb),
               {{VK_CONTROL, false}, {VK_MENU, false}, {VK_DELETE, false, true}, {VK_DELETE, true, true},
                {VK_MENU, true}, {VK_CONTROL, true}});
  check_events(ld::combo_inputs(ld::parse_keys("Enter").combos[0], kb), {{VK_RETURN, false}, {VK_RETURN, true}});
  check_events(ld::combo_inputs(ld::parse_keys("NumpadEnter").combos[0], kb),
               {{VK_RETURN, false, true}, {VK_RETURN, true, true}});
  check_events(ld::combo_inputs(ld::parse_keys("MediaNext").combos[0], kb),
               {{VK_MEDIA_NEXT_TRACK, false, true}, {VK_MEDIA_NEXT_TRACK, true, true}});
}

TEST_CASE("combo_inputs: a character's key comes from the layout, with the modifiers it needs") {
  fakes::Keyboard kb;
  check_events(ld::combo_inputs(ld::parse_keys("Ctrl+Minus").combos[0], kb),
               {{VK_CONTROL, false}, {VK_OEM_MINUS, false}, {VK_OEM_MINUS, true}, {VK_CONTROL, true}});
  check_events(ld::combo_inputs(ld::parse_keys("Ctrl+?").combos[0], kb),  // Shift added
               {{VK_CONTROL, false}, {VK_SHIFT, false}, {VK_OEM_2, false}, {VK_OEM_2, true}, {VK_SHIFT, true},
                {VK_CONTROL, true}});
  check_events(ld::combo_inputs(ld::parse_keys("Shift+?").combos[0], kb),  // not twice
               {{VK_SHIFT, false}, {VK_OEM_2, false}, {VK_OEM_2, true}, {VK_SHIFT, true}});
  check_events(ld::combo_inputs(ld::parse_keys("\xE2\x82\xAC").combos[0], kb),  // € is AltGr+E here
               {{VK_CONTROL, false}, {VK_MENU, false}, {'E', false}, {'E', true}, {VK_MENU, true}, {VK_CONTROL, true}});
  CHECK(ld::combo_inputs(ld::parse_keys("Ctrl+;").combos[0], kb).empty());  // no such key on this layout
}

TEST_CASE("text_inputs: Unicode characters, an emoji as its two surrogates, Enter and Tab") {
  const std::vector<INPUT> in = ld::text_inputs(L"a\U0001F600\r\nb\tc\n");
  // a, the two surrogates, Enter, b, Tab, c, Enter: 8 keys, each down then up.
  REQUIRE(in.size() == 16);
  auto unicode = [&](std::size_t i, wchar_t unit) {
    INFO("event " << i);
    CHECK(in[i].ki.dwFlags == KEYEVENTF_UNICODE);
    CHECK(in[i].ki.wScan == unit);
    CHECK(in[i].ki.wVk == 0);
    CHECK(in[i + 1].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
    CHECK(in[i + 1].ki.wScan == unit);
  };
  auto key = [&](std::size_t i, std::uint16_t v) {
    INFO("event " << i);
    CHECK(in[i].ki.wVk == v);
    CHECK(in[i].ki.dwFlags == 0);
    CHECK(in[i + 1].ki.wVk == v);
    CHECK(in[i + 1].ki.dwFlags == KEYEVENTF_KEYUP);
  };
  unicode(0, L'a');
  unicode(2, 0xD83D);
  unicode(4, 0xDE00);
  key(6, VK_RETURN);
  unicode(8, L'b');
  key(10, VK_TAB);
  unicode(12, L'c');
  key(14, VK_RETURN);
}

TEST_CASE("hotkey: schema defaults, and a syntax error refused with a precise message (422)") {
  const auto info = ld::hotkey_module_info();
  CHECK(ld::element_spec_errors(info.elements).empty());
  json s;
  CHECK_FALSE(ld::apply_schema(info.settings_schema, json::object(), s));
  CHECK(s["mode"] == "combo");
  CHECK(s["keys"] == "");
  CHECK(s["delay_ms"] == 50);
  CHECK(s["icon_source"] == "glyph");
  CHECK(ld::apply_schema(info.settings_schema, {{"delay_ms", 2001}}, s).has_value());
  CHECK(ld::apply_schema(info.settings_schema, {{"icon_source", "program"}}, s).has_value());
  REQUIRE(info.check);
  CHECK_FALSE(ld::apply_schema(info.settings_schema, {{"keys", "Ctrlx"}}, s));
  CHECK(info.check(s) == "keys: unknown key \"Ctrlx\"");
  s["keys"] = "Ctrl+Shift+M, Enter";
  CHECK_FALSE(info.check(s));
  s["mode"] = "text";
  s["keys"] = "Ctrlx";  // not checked in text mode
  s["text"] = std::string(1000, 'x');
  CHECK_FALSE(info.check(s));
  std::string emoji;  // 1000 emoji: 4000 bytes, 1000 characters
  for (int i = 0; i < 1000; ++i) emoji += "\xF0\x9F\x98\x80";
  s["text"] = emoji;
  CHECK_FALSE(info.check(s));
  s["text"] = emoji + "x";
  CHECK(info.check(s) == "text: at most 1000 characters");

  // Through the configuration, as a save from the page.
  json keys = json::array();
  for (int i = 0; i < 10; ++i) keys.push_back(nullptr);
  keys[3] = {{"module", "hotkey"}, {"settings", {{"keys", "Ctrl+Shift+Mute"}}}};
  const ld::ParseResult r = ld::parse_config(json{{"version", 1}, {"keys", keys}}, [](std::string_view id) {
    return ld::find_module(id);
  });
  CHECK_FALSE(r.config);
  REQUIRE(r.errors.size() == 1);
  CHECK(r.errors[0].path == "keys[3].settings");
  CHECK(r.errors[0].message == "keys: unknown key \"Mute\"");
}

TEST_CASE("hotkey: a press sends each combination on the worker, with the delay between them") {
  auto k = start({{"keys", "Win+Shift+S, Enter"}, {"delay_ms", 120}});
  CHECK(fixtures::element_of(*k.rig->m, "icon")->text == L"⌨️");  // ⌨️
  CHECK(fixtures::element_of(*k.rig->m, "label")->text == L"Win+Shift+S, Enter");
  k.press();
  CHECK(k.key().flashing());
  CHECK(k.keyboard->sent.empty());  // nothing on the loop thread
  k.press();  // in flight: ignored
  CHECK(k.held.jobs->size() == 1);
  k.finish();
  REQUIRE(k.keyboard->sent.size() == 2);  // one SendInput per combination
  check_events(k.keyboard->sent[0], {{VK_LWIN, false, true}, {VK_SHIFT, false}, {'S', false}, {'S', true},
                                     {VK_SHIFT, true}, {VK_LWIN, true, true}});
  check_events(k.keyboard->sent[1], {{VK_RETURN, false}, {VK_RETURN, true}});
  CHECK(k.keyboard->pauses == std::vector<int>{120});
  CHECK_FALSE(k.key().failing());
  k.press();
  k.finish();
  CHECK(k.keyboard->sent.size() == 4);
}

TEST_CASE("hotkey: text mode types the text as Unicode") {
  auto k = start({{"mode", "text"}, {"text", "Hi \xF0\x9F\x98\x80"}});
  CHECK(fixtures::element_of(*k.rig->m, "label")->text == L"Text");
  k.press();
  k.finish();
  const std::vector<INPUT> all = k.keyboard->all();
  REQUIRE(all.size() == 10);  // H, i, space, two surrogates
  CHECK(all[0].ki.wScan == L'H');
  CHECK(all[6].ki.wScan == 0xD83D);
  CHECK(all[8].ki.wScan == 0xDE00);
  CHECK(k.keyboard->pauses.empty());
}

TEST_CASE("hotkey: not configured, the key says so and a press sends nothing") {
  auto combo = start(json::object());
  CHECK(fixtures::element_of(*combo.rig->m, "title")->text == L"Keys?");
  combo.press();
  combo.finish();
  CHECK(combo.keyboard->sent.empty());
  auto text = start({{"mode", "text"}});
  CHECK(fixtures::element_of(*text.rig->m, "title")->text == L"Text?");
}

TEST_CASE("hotkey: a blocked SendInput shows the \"!\", is logged once, and the rest is not sent") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_hotkey_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  {
    auto k = start({{"keys", "Ctrl+A, Ctrl+B"}});
    k.keyboard->accept = 0;  // UIPI: an elevated window has the focus
    k.press();
    k.finish();
    CHECK(k.keyboard->sent.size() == 1);  // stopped after the first combination
    CHECK(k.key().failing());
    k.press();
    k.finish();
    auto t = start({{"mode", "text"}, {"text", "my secret text"}});
    t.keyboard->accept = 3;
    t.press();
    t.finish();
    CHECK(t.key().failing());
  }
  ld::log_init({}, ld::LogLevel::Info);
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  const std::size_t first = log.find("hotkey: SendInput blocked");
  REQUIRE(first != std::string::npos);
  CHECK(log.find("hotkey: SendInput blocked", first + 1) == std::string::npos);  // once per run
  CHECK(log.find("secret") == std::string::npos);
  CHECK(log.find("Ctrl+A") == std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("hotkey: a character without a key on the layout fails the press") {
  auto k = start({{"keys", "Ctrl+;"}});
  k.press();
  k.finish();
  CHECK(k.keyboard->sent.empty());
  CHECK(k.key().failing());
}

TEST_CASE("releases_after: the keys pressed and not released, the last first") {
  fakes::Keyboard kb;
  const std::vector<INPUT> in = ld::combo_inputs(ld::parse_keys("Ctrl+Shift+A").combos[0], kb);
  check_events(ld::releases_after(std::span<const INPUT>(in.data(), 2)), {{VK_SHIFT, true}, {VK_CONTROL, true}});
  check_events(ld::releases_after(std::span<const INPUT>(in.data(), 4)), {{VK_SHIFT, true}, {VK_CONTROL, true}});
  check_events(ld::releases_after(std::span<const INPUT>(in.data(), 3)),
               {{'A', true}, {VK_SHIFT, true}, {VK_CONTROL, true}});
  CHECK(ld::releases_after(in).empty());
  CHECK(ld::releases_after(std::span<const INPUT>()).empty());
  CHECK(ld::releases_after(ld::text_inputs(L"ab")).empty());  // Unicode events press no key
  const std::vector<INPUT> win = ld::combo_inputs(ld::parse_keys("Win+Left").combos[0], kb);
  check_events(ld::releases_after(std::span<const INPUT>(win.data(), 1)), {{VK_LWIN, true, true}});  // extended kept
}

TEST_CASE("hotkey: a SendInput cut short releases the modifiers it pressed") {
  auto k = start({{"keys", "Ctrl+Shift+A, B"}});
  k.keyboard->accept = 2;  // Ctrl and Shift went down, then the input was blocked
  k.press();
  k.finish();
  REQUIRE(k.keyboard->sent.size() == 2);  // the combination, then the releases; B is not sent
  check_events(k.keyboard->sent[1], {{VK_SHIFT, true}, {VK_CONTROL, true}});
  CHECK(k.key().failing());
}

TEST_CASE("chunk_end: a chunk never splits the two surrogates of a character") {
  // 99 letters then an emoji: units 0-98, then the pair as units 99 and 100 (events 198-201).
  const std::vector<INPUT> in = ld::text_inputs(std::wstring(99, L'a') + L"\U0001F600");
  REQUIRE(in.size() == 202);
  CHECK(ld::chunk_end(in, 0, 200) == 198);  // ends before the high surrogate
  CHECK(ld::chunk_end(in, 198, 200) == 202);
  const std::vector<INPUT> plain = ld::text_inputs(std::wstring(150, L'a'));
  CHECK(ld::chunk_end(plain, 0, 200) == 200);
  CHECK(ld::chunk_end(plain, 200, 200) == 300);
  auto k = start({{"mode", "text"}, {"text", std::string(99, 'a') + "\xF0\x9F\x98\x80"}});
  k.press();
  k.finish();
  REQUIRE(k.keyboard->sent.size() == 2);
  CHECK(k.keyboard->sent[0].size() == 198);
  CHECK(k.keyboard->sent[1].size() == 4);
}

TEST_CASE("hotkey: a sequence stops when its key is removed, and the layout is taken once per press") {
  auto k = start({{"keys", "A, B, C"}, {"delay_ms", 500}});
  k.press();
  k.rig->m.reset();  // the key is removed (a config change) while the sequence waits for its worker
  k.held.run_all();
  CHECK(k.keyboard->sent.empty());
  auto once = start({{"keys", "A, B"}});
  once.press();
  once.finish();
  CHECK(once.keyboard->begins == 1);
  CHECK(once.keyboard->sent.size() == 2);
}

TEST_CASE("hotkey: a sequence that never ends times out and the key takes presses again") {
  auto k = start({{"keys", "A"}});
  k.press();
  k.rig->loop.advance(ld::kActionTimeout);
  CHECK(k.key().failing());
  CHECK_FALSE(k.key().busy());
  k.held.run_all();  // the late job: cancelled, it sends nothing
  CHECK(k.keyboard->sent.empty());
}
