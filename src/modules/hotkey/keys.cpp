// src/modules/hotkey/keys.cpp
#include "modules/hotkey/keys.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <format>
#include <thread>
#include "core/log.h"
#include "core/strings.h"

namespace ld {
namespace {

struct NamedKey {
  const char* name;  // lower case
  const char* display;
  std::uint16_t vk;
  bool extended;
};

constexpr NamedKey kModifiers[] = {
    {"ctrl", "Ctrl", VK_CONTROL, false}, {"control", "Ctrl", VK_CONTROL, false}, {"alt", "Alt", VK_MENU, false},
    {"shift", "Shift", VK_SHIFT, false}, {"win", "Win", VK_LWIN, true},          {"windows", "Win", VK_LWIN, true},
};

constexpr NamedKey kKeys[] = {
    {"enter", "Enter", VK_RETURN, false},
    {"return", "Enter", VK_RETURN, false},
    {"esc", "Esc", VK_ESCAPE, false},
    {"escape", "Esc", VK_ESCAPE, false},
    {"tab", "Tab", VK_TAB, false},
    {"space", "Space", VK_SPACE, false},
    {"backspace", "Backspace", VK_BACK, false},
    {"delete", "Delete", VK_DELETE, true},
    {"del", "Delete", VK_DELETE, true},
    {"insert", "Insert", VK_INSERT, true},
    {"ins", "Insert", VK_INSERT, true},
    {"home", "Home", VK_HOME, true},
    {"end", "End", VK_END, true},
    {"pageup", "PageUp", VK_PRIOR, true},
    {"pgup", "PageUp", VK_PRIOR, true},
    {"pagedown", "PageDown", VK_NEXT, true},
    {"pgdn", "PageDown", VK_NEXT, true},
    {"up", "Up", VK_UP, true},
    {"down", "Down", VK_DOWN, true},
    {"left", "Left", VK_LEFT, true},
    {"right", "Right", VK_RIGHT, true},
    {"printscreen", "PrintScreen", VK_SNAPSHOT, true},
    {"prtsc", "PrintScreen", VK_SNAPSHOT, true},
    {"pause", "Pause", VK_PAUSE, false},
    {"capslock", "CapsLock", VK_CAPITAL, false},
    {"numlock", "NumLock", VK_NUMLOCK, true},
    {"scrolllock", "ScrollLock", VK_SCROLL, false},
    {"apps", "Apps", VK_APPS, true},
    {"menu", "Apps", VK_APPS, true},
    {"numpadadd", "NumpadAdd", VK_ADD, false},
    {"numpadsubtract", "NumpadSubtract", VK_SUBTRACT, false},
    {"numpadmultiply", "NumpadMultiply", VK_MULTIPLY, false},
    {"numpaddivide", "NumpadDivide", VK_DIVIDE, true},
    {"numpaddecimal", "NumpadDecimal", VK_DECIMAL, false},
    {"numpadenter", "NumpadEnter", VK_RETURN, true},
    {"mediaplaypause", "MediaPlayPause", VK_MEDIA_PLAY_PAUSE, true},
    {"medianext", "MediaNext", VK_MEDIA_NEXT_TRACK, true},
    {"mediaprev", "MediaPrev", VK_MEDIA_PREV_TRACK, true},
    {"mediaprevious", "MediaPrev", VK_MEDIA_PREV_TRACK, true},
    {"mediastop", "MediaStop", VK_MEDIA_STOP, true},
    {"volumeup", "VolumeUp", VK_VOLUME_UP, true},
    {"volumedown", "VolumeDown", VK_VOLUME_DOWN, true},
    {"volumemute", "VolumeMute", VK_VOLUME_MUTE, true},
};

// Punctuation by name: the character, whose key the layout decides when it is sent.
struct NamedChar {
  const char* name;
  wchar_t ch;
};
constexpr NamedChar kChars[] = {
    {"minus", L'-'},     {"plus", L'+'},       {"comma", L','},       {"period", L'.'},
    {"dot", L'.'},       {"slash", L'/'},      {"backslash", L'\\'},  {"semicolon", L';'},
    {"quote", L'\''},    {"backquote", L'`'},  {"grave", L'`'},       {"bracketleft", L'['},
    {"bracketright", L']'}, {"equals", L'='},  {"equal", L'='},
};

std::string trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t at = s.find(sep, start);
    out.push_back(trim(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start)));
    if (at == std::string_view::npos) return out;
    start = at + 1;
  }
}

struct Token {
  KeyCode code;
  std::string display;
  bool modifier = false;
};

std::optional<Token> lookup(const std::string& token) {
  const std::string l = lower(token);
  for (const NamedKey& k : kModifiers)
    if (l == k.name) return Token{KeyCode{.vk = k.vk, .extended = k.extended}, k.display, true};
  for (const NamedKey& k : kKeys)
    if (l == k.name) return Token{KeyCode{.vk = k.vk, .extended = k.extended}, k.display, false};
  for (const NamedChar& c : kChars)
    if (l == c.name) {
      std::string d = c.name;
      d[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(d[0])));
      return Token{KeyCode{.ch = c.ch}, d, false};
    }
  if (l.size() == 1 && std::isalpha(static_cast<unsigned char>(l[0])))
    return Token{KeyCode{.vk = static_cast<std::uint16_t>(std::toupper(static_cast<unsigned char>(l[0])))},
                 std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(l[0])))), false};
  if (l.size() == 1 && std::isdigit(static_cast<unsigned char>(l[0])))
    return Token{KeyCode{.vk = static_cast<std::uint16_t>(l[0])}, l, false};
  if (l.size() >= 2 && l.size() <= 3 && l[0] == 'f' &&
      std::all_of(l.begin() + 1, l.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
    const int n = std::stoi(l.substr(1));
    if (n >= 1 && n <= 24) return Token{KeyCode{.vk = static_cast<std::uint16_t>(VK_F1 + n - 1)}, "F" + std::to_string(n), false};
  }
  if (l.size() == 7 && l.starts_with("numpad") && std::isdigit(static_cast<unsigned char>(l[6])))
    return Token{KeyCode{.vk = static_cast<std::uint16_t>(VK_NUMPAD0 + (l[6] - '0'))}, "Numpad" + l.substr(6), false};
  const std::wstring w = widen(token);  // one printable character other than a letter or a digit
  if (w.size() == 1 && !std::iswspace(w[0]) && !std::iswcntrl(w[0])) return Token{KeyCode{.ch = w[0]}, token, false};
  return std::nullopt;
}

}  // namespace

KeyParse parse_keys(std::string_view text) {
  KeyParse r;
  const std::string all = trim(text);
  if (all.empty()) return r;
  const std::vector<std::string> combos = split(all, ',');
  if (combos.size() > kMaxCombos) {
    r.error = std::format("keys: at most {} combinations", kMaxCombos);
    return r;
  }
  auto fail = [&r](std::string e) {
    r.combos.clear();
    r.display.clear();
    r.error = "keys: " + std::move(e);
    return r;
  };
  for (const std::string& c : combos) {
    if (c.empty()) return fail("an empty combination (two commas, or a comma at an end)");
    KeyCombo combo;
    std::vector<std::string> shown;
    std::optional<Token> key;
    for (const std::string& t : split(c, '+')) {
      if (t.empty()) return fail(std::format("a missing key in \"{}\" (write Plus for the + key)", c));
      std::optional<Token> tok = lookup(t);
      if (!tok) return fail(std::format("unknown key \"{}\"", t));
      if (key) {
        if (tok->modifier) return fail(std::format("the modifiers come before the key in \"{}\"", c));
        return fail(std::format("two keys in \"{}\"", c));
      }
      if (tok->modifier) {
        if (std::find(combo.modifiers.begin(), combo.modifiers.end(), tok->code) != combo.modifiers.end())
          return fail(std::format("\"{}\" twice in \"{}\"", tok->display, c));
        combo.modifiers.push_back(tok->code);
      } else {
        key = tok;
      }
      shown.push_back(tok->display);
    }
    if (key) {
      combo.key = key->code;
    } else {  // modifiers only: the last one is the key ("Win")
      combo.key = combo.modifiers.back();
      combo.modifiers.pop_back();
    }
    r.combos.push_back(std::move(combo));
    std::string d;
    for (const std::string& s : shown) d += (d.empty() ? "" : "+") + s;
    r.display += (r.display.empty() ? "" : ", ") + d;
  }
  return r;
}

namespace {

INPUT key_event(std::uint16_t vk, std::uint16_t scan, bool extended, bool up) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.wScan = scan;
  in.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
  return in;
}

INPUT unicode_event(wchar_t unit, bool up) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wScan = unit;
  in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
  return in;
}

}  // namespace

std::vector<INPUT> combo_inputs(const KeyCombo& combo, Keyboard& kb) {
  std::vector<KeyCode> mods = combo.modifiers;
  KeyCode key = combo.key;
  if (key.ch) {
    const std::optional<Keyboard::CharKey> ck = kb.key_for_char(key.ch);
    if (!ck) return {};
    key = KeyCode{.vk = ck->vk};
    auto need = [&mods](std::uint16_t vk, bool wanted) {
      if (wanted && std::none_of(mods.begin(), mods.end(), [vk](const KeyCode& m) { return m.vk == vk; }))
        mods.push_back(KeyCode{.vk = vk});
    };
    need(VK_CONTROL, ck->ctrl);
    need(VK_MENU, ck->alt);
    need(VK_SHIFT, ck->shift);
  }
  std::vector<INPUT> out;
  out.reserve(mods.size() * 2 + 2);
  for (const KeyCode& m : mods) out.push_back(key_event(m.vk, kb.scan_code(m.vk), m.extended, false));
  const std::uint16_t scan = kb.scan_code(key.vk);
  out.push_back(key_event(key.vk, scan, key.extended, false));
  out.push_back(key_event(key.vk, scan, key.extended, true));
  for (auto it = mods.rbegin(); it != mods.rend(); ++it) out.push_back(key_event(it->vk, kb.scan_code(it->vk), it->extended, true));
  return out;
}

std::vector<INPUT> text_inputs(std::wstring_view text) {
  std::vector<INPUT> out;
  out.reserve(text.size() * 2);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c == L'\r' || c == L'\n') {
      if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;  // "\r\n": one Enter
      out.push_back(key_event(VK_RETURN, 0, false, false));
      out.push_back(key_event(VK_RETURN, 0, false, true));
    } else if (c == L'\t') {
      out.push_back(key_event(VK_TAB, 0, false, false));
      out.push_back(key_event(VK_TAB, 0, false, true));
    } else {
      out.push_back(unicode_event(c, false));
      out.push_back(unicode_event(c, true));
    }
  }
  return out;
}

std::vector<INPUT> releases_after(std::span<const INPUT> sent) {
  std::vector<INPUT> down;  // pressed and not released yet, in order
  for (const INPUT& in : sent) {
    if (in.type != INPUT_KEYBOARD || (in.ki.dwFlags & KEYEVENTF_UNICODE)) continue;
    auto same = [&in](const INPUT& d) { return d.ki.wVk == in.ki.wVk; };
    if (in.ki.dwFlags & KEYEVENTF_KEYUP) std::erase_if(down, same);
    else if (std::none_of(down.begin(), down.end(), same)) down.push_back(in);
  }
  std::vector<INPUT> out;
  for (auto it = down.rbegin(); it != down.rend(); ++it) {
    INPUT up = *it;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    out.push_back(up);
  }
  return out;
}

std::size_t chunk_end(std::span<const INPUT> events, std::size_t begin, std::size_t max) {
  std::size_t end = std::min(events.size(), begin + max);
  if (end < events.size() && end - begin > 2) {
    const INPUT& last = events[end - 1];  // the up of the last unit in the chunk
    if ((last.ki.dwFlags & KEYEVENTF_UNICODE) && last.ki.wScan >= 0xD800 && last.ki.wScan <= 0xDBFF) end -= 2;
  }
  return end;
}

namespace {

class SystemKeyboard final : public Keyboard {
public:
  unsigned send(std::span<const INPUT> inputs) override {
    if (inputs.empty()) return 0;
    return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()), sizeof(INPUT));
  }
  void begin() override {
    // The layout of the window that receives the keys, not this worker's.
    layout_ = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
  }
  std::optional<CharKey> key_for_char(wchar_t ch) override {
    const SHORT r = VkKeyScanExW(ch, layout_);
    if (r == -1 || LOBYTE(r) == 0xFF) return std::nullopt;
    const BYTE mods = HIBYTE(r);
    return CharKey{.vk = LOBYTE(r), .shift = (mods & 1) != 0, .ctrl = (mods & 2) != 0, .alt = (mods & 4) != 0};
  }
  std::uint16_t scan_code(std::uint16_t vk) override {
    if (vk == VK_SNAPSHOT) return 0x37;  // E0 37 (the mapping may give 0x54, SysRq)
    return static_cast<std::uint16_t>(MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout_) & 0xFF);
  }
  void pause(int ms) override {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));  // the hotkey's own worker
  }

private:
  HKL layout_ = GetKeyboardLayout(0);
};

}  // namespace

std::shared_ptr<Keyboard> system_keyboard() { return std::make_shared<SystemKeyboard>(); }

}  // namespace ld
