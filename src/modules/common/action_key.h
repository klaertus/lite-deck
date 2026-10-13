// src/modules/common/action_key.h: the shared base of the keys that do something when pressed
// (launcher, hotkey): an icon (a program's, an image file, a glyph) or a large title, a label, a
// short flash when pressed and a red "!" when the action fails.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "modules/common/key_look.h"
#include "modules/common/worker.h"
#include "modules/key_module.h"

namespace ld {

// Where the icon of an action key comes from (setting `icon_source`).
enum class IconSource { Program, Image, Glyph, None };
std::optional<IconSource> icon_source_from_name(std::string_view name);  // "program", "image", "glyph", "none"

// Icons are decoded to fit a square of this many pixels, then scaled to the element's size.
inline constexpr int kIconPixels = 96;

struct IconRequest {
  IconSource source = IconSource::None;  // Program or Image
  std::wstring path;  // Program: an exe, a file, a folder or a URL (its browser's icon); Image: a picture
  int size = kIconPixels;
  bool operator==(const IconRequest&) const = default;
};

// Loads an icon. Runs on a worker thread and may block (shell, disk, decoding).
class IconLoader {
public:
  virtual ~IconLoader() = default;
  // The icon fitted in size x size with its proportions; nullptr when it cannot be found or read.
  virtual std::shared_ptr<const Bitmap> load(const IconRequest& request) = 0;
};
// The shell's icons (IShellItemImageFactory, then SHGetFileInfo) and pictures decoded by WIC (PNG,
// JPG, ICO, BMP), kept in one IconCache keyed by path, modification time and size. Process-wide.
std::shared_ptr<IconLoader> system_icon_loader();

// A bounded LRU of decoded icons, thread-safe: at most `max_entries` icons and `max_bytes` of pixels
// (the oldest go first; an icon larger than `max_bytes` is never kept).
class IconCache {
public:
  explicit IconCache(std::size_t max_entries = 64, std::size_t max_bytes = 4u << 20);
  std::shared_ptr<const Bitmap> get(const std::wstring& key);
  void put(const std::wstring& key, std::shared_ptr<const Bitmap> icon);
  std::size_t size() const;
  std::size_t bytes() const;

private:
  struct Entry {
    std::wstring key;
    std::shared_ptr<const Bitmap> icon;
  };
  mutable std::mutex mu_;
  std::size_t max_entries_, max_bytes_, bytes_ = 0;
  std::list<Entry> lru_;  // most recently used first
  std::map<std::wstring, std::list<Entry>::iterator, std::less<>> index_;
};

// A path as the user pasted it: trimmed, and one pair of surrounding double quotes removed
// (Explorer's "Copy as path" adds them).
std::wstring unquote(std::wstring_view text);
// A bare program name ("notepad.exe") found on the PATH, never in the current directory; "" when
// not found.
std::wstring find_on_path(std::wstring_view name);
// `%NAME%` environment variables expanded (ExpandEnvironmentStringsW); unknown ones stay as written.
std::wstring expand_env(std::wstring_view text);
// A URL rather than a path: a scheme of two letters or more before ':' ("https:", "mailto:"), so
// a drive ("C:") stays a path.
bool is_url(std::wstring_view text);
// What a log line may say about a path or a URL: the file name, or "a <scheme> link".
std::string name_for_log(std::wstring_view path_or_url);
// The kind of a target only, never its name: "a https link", "a .xlsx file" or "a folder".
std::string kind_for_log(std::wstring_view path_or_url);

// What the glyph setting draws: an emoji (Segoe UI, in colour) or one glyph of an icon font, typed
// as the character or as its code ("U+E713", ""). A code point of the Private Use Area
// (U+E000-U+F8FF) is an icon font's: `font` is then Segoe Fluent Icons, or Segoe MDL2 Assets
// where Fluent is not installed.
struct Glyph {
  std::wstring text;
  std::wstring font;  // "" = Segoe UI
};
Glyph parse_glyph(std::string_view setting);

// The three elements of an action key in the module's hue: `icon` (middle), `title` (middle, the
// text alone when the icon source is "none") and `label` (bottom, grey).
constexpr std::array<ElementSpec, 3> action_elements(Rgb hue) {
  return {{
      {.key = "icon", .label = "Icon", .size = 48, .weight = 400, .min_size = 12, .max_size = 80, .icon = true,
       .kind = ElementKind::Icon, .row = Row::Middle, .color = hue},
      {.key = "title", .label = "Title", .size = 26, .weight = 700, .min_size = 10, .max_size = 60, .row = Row::Middle,
       .color = hue, .role = TextRole::Value},
      {.key = "label", .label = "Label", .size = 15, .weight = 600, .min_size = 8, .max_size = 30, .row = Row::Bottom,
       .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
  }};
}

// The settings every action key shares: icon_source (`sources`, the first being the default),
// image, glyph, glyph_color, label and title.
nlohmann::json action_fields(const std::vector<std::string>& sources, const char* default_glyph, Rgb glyph_color,
                             const char* label_hint);

// What the action key's systems are: tests pass fakes and an inline runner.
struct ActionDeps {
  std::shared_ptr<IconLoader> icons;  // null: system_icon_loader()
  Runner icon_runner;                 // null: one worker shared by every action key
  Runner runner;                      // the actions (a launch, keystrokes); null: worker_runner()
};

// How long the key lights up when pressed, and how long the "!" of a failure stays.
inline constexpr Duration kPressFlash{150};
inline constexpr Duration kFailureShown{1500};
// An action that has not answered after this is given up: the key shows "!" and takes presses again.
inline constexpr Duration kActionTimeout{30000};
// After this, a slow icon (a sleeping network drive) shows the generic pictogram until it arrives.
inline constexpr Duration kIconPatience{2000};

// Set when the action in flight must stop: it timed out, or its key was removed.
using CancelToken = std::shared_ptr<std::atomic<bool>>;

// What a key that does something when pressed shows and keeps: the press flash, the "!" of a
// failure, and the action in flight with its timeout. ActionKey has one; a monitor key's tap
// action (TapKey, modules/common/tap_action.h) has one too. Loop thread only.
class ActionFeedback {
public:
  ActionFeedback() = default;
  ActionFeedback(const ActionFeedback&) = delete;
  ActionFeedback& operator=(const ActionFeedback&) = delete;
  ~ActionFeedback();  // cancels the action in flight

  // Before any of the calls below; `log_name` starts the timeout's log line.
  void attach(ModuleContext& ctx, const char* log_name = "action key") {
    ctx_ = &ctx;
    log_name_ = log_name;
  }
  void pressed();  // lights the key up for kPressFlash
  void failed();   // shows the "!" for kFailureShown
  // A press starts an action: nullopt while one is in flight (the press is ignored); else the key
  // lights up and the action has kActionTimeout to end. Pass the token to the job, which stops
  // when it is set, and to end().
  std::optional<CancelToken> begin();
  // From the posted result: false when that action is no longer current (it timed out).
  bool end(const CancelToken& token);
  // The flash and the "!" over what the key drew; nothing outside their windows, so the key's
  // normal image is unchanged.
  void draw(Canvas& c) const;
  bool flashing() const { return flash_timer_.has_value(); }
  bool failing() const { return fail_timer_.has_value(); }
  bool busy() const { return token_ != nullptr; }

private:
  ModuleContext* ctx_ = nullptr;
  const char* log_name_ = "action key";
  CancelToken token_;
  std::optional<TimerId> timeout_timer_, flash_timer_, fail_timer_;
};

class ActionKey : public KeyModule {
public:
  std::vector<KeyElement> content() override;
  void render(Canvas& c) override;  // the elements, then the press flash and the "!" over them
  bool flashing() const { return feedback_.flashing(); }
  bool failing() const { return feedback_.failing(); }
  bool busy() const { return feedback_.busy(); }

protected:
  ActionKey(std::span<const ElementSpec> elements, ActionDeps deps);

  // What the subclass found in its settings.
  struct Target {
    std::wstring program;        // icon_source "program": the exe, file, folder or URL ("" = none)
    bool url = false;            // `program` is a URL: 🌐 when its browser has no icon
    std::wstring default_label;  // the program's name, the URL's host, the keys
    std::wstring hint;           // not "" = not configured: shown alone, idle ("Path?")
    std::string log_name;        // how the log names `program` ("" = name_for_log(program))
  };
  // Reads the shared settings and starts loading the icon. Call from start().
  void start_action(ModuleContext& ctx, const nlohmann::json& settings, Target target);
  void pressed() { feedback_.pressed(); }
  void failed() { feedback_.failed(); }
  // ActionFeedback::begin() and end().
  std::optional<CancelToken> begin_action() { return feedback_.begin(); }
  bool end_action(const CancelToken& token) { return feedback_.end(token); }
  ModuleContext* ctx_ = nullptr;
  Runner runner_;

private:
  void on_icon(std::shared_ptr<const Bitmap> icon);
  ActionDeps deps_;
  IconSource source_ = IconSource::None;
  Target target_;
  Glyph glyph_;
  Rgb glyph_color_{};
  std::wstring label_, title_;
  bool loading_ = false;
  bool icon_late_ = false;  // still loading after kIconPatience: the fallback shows meanwhile
  std::shared_ptr<const Bitmap> icon_;
  Glyph fallback_;  // drawn when the icon cannot be found
  ActionFeedback feedback_;
};

}  // namespace ld
