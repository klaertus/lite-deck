// src/modules/hotkey/hotkey.cpp
#include "modules/hotkey/hotkey.h"
#include <atomic>
#include <format>
#include "core/log.h"
#include "core/strings.h"

namespace ld {
namespace {

constexpr Rgb kMagenta{0xff, 0x4f, 0xd8};
constexpr auto kElements = action_elements(kMagenta);
constexpr std::size_t kChunk = 200;  // events per SendInput call when typing a text

std::size_t code_points(std::string_view utf8) {
  std::size_t n = 0;
  for (unsigned char c : utf8) n += (c & 0xC0) != 0x80;
  return n;
}

// The first `max` code points of `text`.
std::wstring first_code_points(const std::wstring& text, std::size_t max) {
  std::size_t n = 0, i = 0;
  for (; i < text.size() && n < max; ++i, ++n)
    if (text[i] >= 0xD800 && text[i] <= 0xDBFF && i + 1 < text.size()) ++i;  // a surrogate pair
  return text.substr(0, i);
}

// Why a sequence was not sent, for the log (never the keys or the text).
enum class Failure { None, Blocked, NoKey, Error };

class HotkeyModule final : public ActionKey {
public:
  HotkeyModule(std::shared_ptr<Keyboard> keyboard, ActionDeps deps)
      : ActionKey(kElements, std::move(deps)), keyboard_(keyboard ? std::move(keyboard) : system_keyboard()) {}

  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    Target t;
    text_mode_ = s.at("mode").get<std::string>() == "text";
    delay_ms_ = s.at("delay_ms").get<int>();
    if (text_mode_) {
      text_ = first_code_points(widen(s.at("text").get<std::string>()), kMaxTypedText);
      t.default_label = L"Text";
      if (text_.empty()) t.hint = L"Text?";
    } else {
      KeyParse p = parse_keys(s.at("keys").get<std::string>());
      combos_ = std::move(p.combos);
      t.default_label = widen(p.display);
      if (combos_.empty()) t.hint = L"Keys?";  // empty (a syntax error never reaches start: check refuses it)
    }
    configured_ = t.hint.empty();
    start_action(ctx, s, std::move(t));
  }

  void on_event(const InputEvent& e) override {
    if (e.kind != EventKind::Press || !configured_) return;
    const std::optional<CancelToken> token = begin_action();
    if (!token) return;  // a sequence in flight: this press is ignored
    runner_([kb = keyboard_, combos = combos_, text = text_, text_mode = text_mode_, delay = delay_ms_,
             token = *token, p = ctx_->poster(), this] {
      Failure failure = Failure::None;
      DWORD error = 0;
      // One call per chunk; a call cut short releases what it pressed, so no modifier stays down.
      auto send = [&](const std::vector<INPUT>& in) {
        for (std::size_t i = 0; i < in.size() && failure == Failure::None;) {
          if (token->load()) return;  // the key was removed, or the sequence timed out
          const std::size_t end = chunk_end(in, i, kChunk);
          const std::span<const INPUT> part(in.data() + i, end - i);
          const unsigned taken = kb->send(part);
          if (taken != part.size()) {
            failure = Failure::Blocked;
            error = GetLastError();
            const std::vector<INPUT> up = releases_after(std::span<const INPUT>(in.data(), i + std::min<std::size_t>(taken, part.size())));
            if (!up.empty()) kb->send(up);  // best effort
          }
          i = end;
        }
      };
      try {
        kb->begin();
        if (text_mode) {
          send(text_inputs(text));
        } else {
          for (std::size_t i = 0; i < combos.size() && failure == Failure::None && !token->load(); ++i) {
            if (i > 0) kb->pause(delay);
            if (token->load()) break;
            const std::vector<INPUT> in = combo_inputs(combos[i], *kb);
            if (in.empty()) failure = Failure::NoKey;
            else send(in);
          }
        }
      } catch (...) {
        failure = Failure::Error;
      }
      p.post([this, token, failure, error] { on_done(token, failure, error); });  // always: never busy forever
    });
  }

private:
  void on_done(const CancelToken& token, Failure failure, DWORD error) {
    if (!end_action(token) || failure == Failure::None) return;
    static std::atomic<bool> warned{false};  // once per run: a focused elevated window blocks every key
    if (failure == Failure::Blocked && !warned.exchange(true))
      log_warn("hotkey: SendInput blocked (error {}); an elevated window may have the focus", error);
    if (failure == Failure::NoKey && !no_key_logged_) {
      no_key_logged_ = true;
      log_warn("hotkey: a character of the combination has no key on the current keyboard layout");
    }
    if (failure == Failure::Error) log_warn("hotkey: the keys could not be sent");
    failed();
  }

  std::shared_ptr<Keyboard> keyboard_;
  bool text_mode_ = false;
  int delay_ms_ = 50;
  std::vector<KeyCombo> combos_;
  std::wstring text_;
  bool configured_ = false;
  bool no_key_logged_ = false;
};

std::optional<std::string> check_hotkey(const nlohmann::json& s) {
  if (s.at("mode").get<std::string>() == "text") {
    if (code_points(s.at("text").get<std::string>()) > kMaxTypedText)
      return std::format("text: at most {} characters", kMaxTypedText);
    return std::nullopt;
  }
  KeyParse p = parse_keys(s.at("keys").get<std::string>());
  if (!p.error.empty()) return p.error;
  return std::nullopt;
}

}  // namespace

std::unique_ptr<Module> make_hotkey_module(std::shared_ptr<Keyboard> keyboard, ActionDeps deps) {
  return std::make_unique<HotkeyModule>(std::move(keyboard), std::move(deps));
}

ModuleInfo hotkey_module_info() {
  using nlohmann::json;
  ModuleInfo m;
  m.id = "hotkey";
  m.name = "Hotkey";
  m.description = "Press to send a key shortcut or to type a text.";
  m.category = "Actions";
  m.hue = "#ff4fd8";
  m.slots = {SlotKind::Key};
  json fields = json::array({
      json{{"key", "mode"}, {"type", "enum"}, {"default", "combo"}, {"label", "Send"},
           {"options", json::array({json{{"value", "combo"}, {"label", "Key shortcut(s)"}},
                                    json{{"value", "text"}, {"label", "Typed text"}}})}},
      json{{"key", "keys"}, {"type", "string"}, {"default", ""}, {"label", "Keys"},
           {"hint", "Ctrl, Alt, Shift or Win and a key, joined by +; up to 8 shortcuts, separated by commas. A window "
                    "run as admin does not get them."},
           {"placeholder", "Ctrl+Shift+M, Enter"}, {"show_if", json{{"field", "mode"}, {"equals", "combo"}}}},
      json{{"key", "delay_ms"}, {"type", "int"}, {"default", 50}, {"min", 0}, {"max", 2000},
           {"label", "Delay between shortcuts (ms)"}, {"show_if", json{{"field", "mode"}, {"equals", "combo"}}}},
      json{{"key", "text"}, {"type", "string"}, {"default", ""}, {"label", "Text"},
           {"hint", "Typed as it is, up to 1000 characters."}, {"show_if", json{{"field", "mode"}, {"equals", "text"}}}},
  });
  for (const json& f : action_fields({"glyph", "image", "none"}, "\xE2\x8C\xA8\xEF\xB8\x8F", kMagenta,
                                     "Empty: the keys."))
    fields.push_back(f);
  m.settings_schema = json{{"fields", fields}};
  m.elements.assign(kElements.begin(), kElements.end());
  m.check = check_hotkey;
  m.create = [] { return make_hotkey_module({}, {}); };
  return m;
}

}  // namespace ld
