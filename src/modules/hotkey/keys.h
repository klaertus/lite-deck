// src/modules/hotkey/keys.h: key combinations ("Ctrl+Shift+M, Enter") and the keyboard events
// that type them.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ld {

// One key: a virtual key, or a punctuation character whose key depends on the keyboard layout
// (looked up when it is sent).
struct KeyCode {
  std::uint16_t vk = 0;
  wchar_t ch = 0;  // not 0: the key that types this character on the current layout
  bool extended = false;
  bool operator==(const KeyCode&) const = default;
};

// Modifiers held, in the order written, around one key.
struct KeyCombo {
  std::vector<KeyCode> modifiers;
  KeyCode key;
  bool operator==(const KeyCombo&) const = default;
};

inline constexpr std::size_t kMaxCombos = 8;
inline constexpr std::size_t kMaxTypedText = 1000;  // characters (code points)

struct KeyParse {
  std::vector<KeyCombo> combos;
  std::string error;  // "" when valid, else `keys: unknown key "Ctrlx"` and the like
  std::string display;  // the combinations as the key shows them: "Ctrl+Shift+M, Enter"
};
// Combinations separated by commas, keys by "+", case-insensitive: modifiers (Ctrl, Alt, Shift,
// Win) then one key; a modifier alone is a key ("Win"). Keys: letters, digits, F1-F24, Enter, Esc,
// Tab, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Up, Down, Left, Right,
// PrintScreen, Pause, CapsLock, NumLock, ScrollLock, Apps, Numpad0-9, NumpadAdd, NumpadSubtract,
// NumpadMultiply, NumpadDivide, NumpadDecimal, NumpadEnter, MediaPlayPause, MediaNext, MediaPrev,
// MediaStop, VolumeUp, VolumeDown, VolumeMute, and punctuation by name (Minus, Plus, Comma,
// Period, Slash, Backslash, Semicolon, Quote, Backquote, BracketLeft, BracketRight, Equals) or as
// the character itself (except "+" and ",", which separate). At most kMaxCombos combinations.
KeyParse parse_keys(std::string_view text);

// The keyboard. Every call runs on the hotkey's worker thread.
class Keyboard {
public:
  virtual ~Keyboard() = default;
  // Called once before each sequence: the system keyboard takes the foreground window's layout
  // here, for both key_for_char and scan_code.
  virtual void begin() {}
  // SendInput: the number of events Windows took (fewer when blocked, such as by UIPI).
  virtual unsigned send(std::span<const INPUT> inputs) = 0;
  // The key and the modifiers that type `ch` on the foreground window's layout; nullopt when none.
  struct CharKey {
    std::uint16_t vk = 0;
    bool shift = false, ctrl = false, alt = false;
  };
  virtual std::optional<CharKey> key_for_char(wchar_t ch) = 0;
  virtual std::uint16_t scan_code(std::uint16_t vk) = 0;  // MapVirtualKeyExW, that same layout
  virtual void pause(int ms) = 0;                          // between two combinations
};
std::shared_ptr<Keyboard> system_keyboard();

// The events of one combination: the modifiers down in order, the key down and up, the
// modifiers up in reverse order. A character's key adds the modifiers its layout needs. Empty
// when a character has no key on the layout.
std::vector<INPUT> combo_inputs(const KeyCombo& combo, Keyboard& keyboard);
// `text` typed as Unicode characters (KEYEVENTF_UNICODE, one down and up per UTF-16 unit, so an
// emoji is its two surrogates); a line break is Enter ("\r\n" once) and a tab is Tab.
std::vector<INPUT> text_inputs(std::wstring_view text);
// After a SendInput that took only `sent` (the first events of a call): the key-ups of every key
// those events pressed and did not release, the last pressed first. Sent at once, best effort, so
// that no modifier stays down. Unicode events need none.
std::vector<INPUT> releases_after(std::span<const INPUT> sent);
// Where to end a chunk of `events` that starts at `begin` and holds at most `max`: never between
// the two surrogates of one character (text_inputs).
std::size_t chunk_end(std::span<const INPUT> events, std::size_t begin, std::size_t max);

}  // namespace ld
