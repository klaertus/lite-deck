// src/modules/common/action_key.cpp
#include "modules/common/action_key.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <map>
#include "core/log.h"
#include "core/strings.h"
#include "render/fonts.h"

namespace ld {

std::optional<IconSource> icon_source_from_name(std::string_view name) {
  if (name == "program") return IconSource::Program;
  if (name == "image") return IconSource::Image;
  if (name == "glyph") return IconSource::Glyph;
  if (name == "none") return IconSource::None;
  return std::nullopt;
}

// --- IconCache -------------------------------------------------------------------------------

IconCache::IconCache(std::size_t max_entries, std::size_t max_bytes) : max_entries_(max_entries), max_bytes_(max_bytes) {}

std::shared_ptr<const Bitmap> IconCache::get(const std::wstring& key) {
  std::lock_guard lock(mu_);
  auto it = index_.find(key);
  if (it == index_.end()) return nullptr;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->icon;
}

void IconCache::put(const std::wstring& key, std::shared_ptr<const Bitmap> icon) {
  if (!icon) return;
  std::lock_guard lock(mu_);
  if (auto it = index_.find(key); it != index_.end()) {
    bytes_ -= it->second->icon->bytes();
    lru_.erase(it->second);
    index_.erase(it);
  }
  if (icon->bytes() > max_bytes_ || max_entries_ == 0) return;
  while (!lru_.empty() && (lru_.size() >= max_entries_ || bytes_ + icon->bytes() > max_bytes_)) {
    bytes_ -= lru_.back().icon->bytes();
    index_.erase(lru_.back().key);
    lru_.pop_back();
  }
  bytes_ += icon->bytes();
  lru_.push_front(Entry{key, std::move(icon)});
  index_.emplace(key, lru_.begin());
}

std::size_t IconCache::size() const {
  std::lock_guard lock(mu_);
  return lru_.size();
}

std::size_t IconCache::bytes() const {
  std::lock_guard lock(mu_);
  return bytes_;
}

// --- Text helpers ----------------------------------------------------------------------------

std::wstring expand_env(std::wstring_view text) {
  const std::wstring in(text);
  if (in.find(L'%') == std::wstring::npos) return in;
  const DWORD n = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
  if (n == 0) return in;
  std::wstring out(n, L'\0');
  if (ExpandEnvironmentStringsW(in.c_str(), out.data(), n) == 0) return in;
  out.resize(n - 1);  // n counts the terminating null
  return out;
}

bool is_url(std::wstring_view t) {
  const std::size_t colon = t.find(L':');
  if (colon == std::wstring_view::npos || colon < 2) return false;  // "C:\..." is a drive
  if (!std::iswalpha(t[0])) return false;
  for (std::size_t i = 1; i < colon; ++i)
    if (!std::iswalnum(t[i]) && t[i] != L'+' && t[i] != L'-' && t[i] != L'.') return false;
  return true;
}

std::wstring unquote(std::wstring_view t) {
  auto trim_ws = [](std::wstring_view v) {
    const auto b = v.find_first_not_of(L" \t\r\n");
    if (b == std::wstring_view::npos) return std::wstring_view();
    return v.substr(b, v.find_last_not_of(L" \t\r\n") - b + 1);
  };
  t = trim_ws(t);
  if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') t = trim_ws(t.substr(1, t.size() - 2));
  return std::wstring(t);
}

std::wstring find_on_path(std::wstring_view name) {
  const std::wstring n(name);
  const DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  std::wstring path(len, L'_');
  if (len == 0 || GetEnvironmentVariableW(L"PATH", path.data(), len) == 0) return {};
  path.resize(len - 1);
  wchar_t buf[MAX_PATH] = {};
  const DWORD found = SearchPathW(path.c_str(), n.c_str(), nullptr, MAX_PATH, buf, nullptr);  // not the current dir
  return found > 0 && found < MAX_PATH ? std::wstring(buf) : std::wstring();
}

std::string kind_for_log(std::wstring_view p) {
  if (is_url(p)) return "a " + narrow(p.substr(0, p.find(L':'))) + " link";
  const std::size_t slash = p.find_last_of(L"\\/");
  const std::wstring_view name = slash == std::wstring_view::npos ? p : p.substr(slash + 1);
  const std::size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring_view::npos || dot == 0 || dot + 1 == name.size()) return "a folder";
  std::wstring ext(name.substr(dot));
  std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return "a " + narrow(ext) + " file";
}

std::string name_for_log(std::wstring_view p) {
  if (is_url(p)) return "a " + narrow(p.substr(0, p.find(L':'))) + " link";
  const std::size_t slash = p.find_last_of(L"\\/");
  std::wstring_view name = slash == std::wstring_view::npos ? p : p.substr(slash + 1);
  if (name.empty() && slash != std::wstring_view::npos) {  // "C:\Folder\": the folder's name
    const std::wstring_view trimmed = p.substr(0, slash);
    const std::size_t s2 = trimmed.find_last_of(L"\\/");
    name = s2 == std::wstring_view::npos ? trimmed : trimmed.substr(s2 + 1);
  }
  return narrow(name);
}

namespace {

std::string trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

std::wstring utf16(char32_t cp) {
  if (cp < 0x10000) return std::wstring(1, static_cast<wchar_t>(cp));
  cp -= 0x10000;
  return {static_cast<wchar_t>(0xD800 + (cp >> 10)), static_cast<wchar_t>(0xDC00 + (cp & 0x3FF))};
}

// "U+E713", "u+e713", "\uE713", "\U0001F680": the code point, or nullopt for anything else.
std::optional<char32_t> glyph_code(const std::string& s) {
  std::string_view hex;
  if (s.size() > 2 && (s[0] == 'U' || s[0] == 'u') && s[1] == '+') hex = std::string_view(s).substr(2);
  else if (s.size() > 2 && s[0] == '\\' && (s[1] == 'u' || s[1] == 'U')) hex = std::string_view(s).substr(2);
  else return std::nullopt;
  if (hex.empty() || hex.size() > 8) return std::nullopt;
  char32_t cp = 0;
  for (char c : hex) {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return std::nullopt;
    cp = cp * 16 + static_cast<char32_t>(d);
  }
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
  return cp;
}

const std::wstring& icon_font() {
  static const std::wstring font = font_installed(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
  return font;
}

Runner shared_icon_runner() {
  static const Runner runner = worker_runner();  // one worker for every action key's icon
  return runner;
}

}  // namespace

Glyph parse_glyph(std::string_view setting) {
  const std::string s = trim(setting);
  Glyph g;
  if (auto cp = glyph_code(s)) g.text = utf16(*cp);
  else g.text = widen(s);
  const bool one_unit = g.text.size() == 1;
  if (one_unit && g.text[0] >= 0xE000 && g.text[0] <= 0xF8FF) g.font = icon_font();
  return g;
}

// --- The schema ------------------------------------------------------------------------------

nlohmann::json action_fields(const std::vector<std::string>& sources, const char* default_glyph, Rgb glyph_color,
                             const char* label_hint) {
  using nlohmann::json;
  static const std::map<std::string, std::string> kIconLabels{
      {"program", "Program icon"}, {"image", "Picture file"}, {"glyph", "Emoji or symbol"}, {"none", "No icon (title only)"}};
  json options = json::array();
  for (const std::string& s : sources) options.push_back(json{{"value", s}, {"label", kIconLabels.at(s)}});
  return json::array({
      json{{"key", "icon_source"}, {"type", "enum"}, {"default", sources.front()}, {"options", options}, {"label", "Icon"}},
      json{{"key", "image"}, {"type", "string"}, {"default", ""}, {"label", "Picture file"},
           {"hint", "A PNG, JPG, ICO or BMP file. %VARIABLES% are expanded."},
           {"placeholder", "%USERPROFILE%\\Pictures\\icon.png"}, {"show_if", json{{"field", "icon_source"}, {"equals", "image"}}}},
      json{{"key", "glyph"}, {"type", "string"}, {"default", default_glyph}, {"label", "Emoji or symbol"},
           {"hint", "An emoji, or a Segoe Fluent Icons symbol by its code (U+E713)."},
           {"show_if", json{{"field", "icon_source"}, {"equals", "glyph"}}}},
      json{{"key", "glyph_color"}, {"type", "color"}, {"default", format_color(glyph_color)}, {"label", "Symbol color"},
           {"show_if", json{{"field", "icon_source"}, {"equals", "glyph"}}}},
      json{{"key", "label"}, {"type", "string"}, {"default", ""}, {"label", "Label"}, {"hint", label_hint}},
      json{{"key", "title"}, {"type", "string"}, {"default", ""}, {"label", "Title"},
           {"hint", "Shown large when there is no icon. Empty: the label."}},
  });
}

// --- ActionKey -------------------------------------------------------------------------------

ActionKey::ActionKey(std::span<const ElementSpec> elements, ActionDeps deps) : KeyModule(elements), deps_(std::move(deps)) {
  if (!deps_.icons) deps_.icons = system_icon_loader();
  if (!deps_.icon_runner) deps_.icon_runner = shared_icon_runner();
  runner_ = deps_.runner ? deps_.runner : worker_runner();
}

void ActionKey::start_action(ModuleContext& ctx, const nlohmann::json& s, Target target) {
  ctx_ = &ctx;
  feedback_.attach(ctx);
  target_ = std::move(target);
  source_ = icon_source_from_name(s.at("icon_source").get<std::string>()).value_or(IconSource::None);
  glyph_ = parse_glyph(s.at("glyph").get<std::string>());
  glyph_color_ = parse_color(s.at("glyph_color").get<std::string>()).value_or(Rgb{255, 255, 255});
  label_ = widen(trim(s.at("label").get<std::string>()));
  title_ = widen(trim(s.at("title").get<std::string>()));
  const std::wstring image = expand_env(unquote(widen(s.at("image").get<std::string>())));
  fallback_.text = source_ == IconSource::Image ? L"\U0001F5BC\uFE0F"           // 🖼️
                   : target_.url                ? L"\U0001F310"                 // 🌐
                                                : L"\U0001F4C4";                // 📄
  IconRequest req{.source = source_};
  if (source_ == IconSource::Program) req.path = target_.program;
  else if (source_ == IconSource::Image) req.path = image;
  ctx.invalidate();
  if (req.path.empty() || !target_.hint.empty()) return;  // nothing to load: the fallback, or the hint
  loading_ = true;
  ctx.at(ctx.now() + kIconPatience, [this] {
    if (!loading_) return;
    icon_late_ = true;  // the fallback until the icon arrives
    ctx_->invalidate();
  });
  deps_.icon_runner([icons = deps_.icons, req, p = ctx.poster(), this] {
    std::shared_ptr<const Bitmap> icon = icons->load(req);
    p.post([this, icon = std::move(icon)] { on_icon(icon); });
  });
}

void ActionKey::on_icon(std::shared_ptr<const Bitmap> icon) {
  loading_ = false;
  icon_ = std::move(icon);
  if (!icon_)  // once per key: the icon is loaded once, at start
    log_info("action key: no icon for {}; showing a generic one",
             source_ == IconSource::Image ? std::string("the image file")
             : !target_.log_name.empty()  ? target_.log_name
                                          : name_for_log(target_.program));
  ctx_->invalidate();
}

std::vector<KeyElement> ActionKey::content() {
  std::vector<KeyElement> out;
  if (!target_.hint.empty()) {
    out.push_back(KeyElement{.key = "title", .text = target_.hint, .state = ElementState::Idle});
    return out;
  }
  const std::wstring& name = label_.empty() ? target_.default_label : label_;
  if (source_ == IconSource::None) {
    out.push_back(KeyElement{.key = "title", .text = !title_.empty() ? title_ : name});
    if (!title_.empty() && !label_.empty()) out.push_back(KeyElement{.key = "label", .text = label_});
    return out;
  }
  KeyElement icon{.key = "icon"};
  if (source_ == IconSource::Glyph) {
    const Glyph& g = glyph_.text.empty() ? fallback_ : glyph_;
    icon.text = g.text;
    icon.font = g.font;
    icon.color = glyph_color_;
  } else if (icon_) {
    icon.image = icon_;
  } else if (!loading_ || icon_late_) {
    icon.text = fallback_.text;
    icon.color = glyph_color_;
  }
  out.push_back(std::move(icon));
  if (!name.empty()) out.push_back(KeyElement{.key = "label", .text = name});
  return out;
}

void ActionKey::render(Canvas& c) {
  KeyModule::render(c);
  feedback_.draw(c);
}

// --- ActionFeedback --------------------------------------------------------------------------

void ActionFeedback::draw(Canvas& c) const {
  if (flashing()) c.fill_rect(RectF{0, 0, c.width(), c.height()}, Rgb{255, 255, 255}, 0.25f);
  if (failing()) {
    const float s = c.height() / kKeyBase;
    TextStyle st{.font = kDefaultFont, .size = 30 * s, .weight = 700, .color = c.look().states.critical};
    c.text(L"!", RectF{c.width() - 30 * s, 0, 28 * s, 30 * s}, st);
  }
}

void ActionFeedback::pressed() {
  if (flash_timer_) ctx_->cancel(*flash_timer_);
  flash_timer_ = ctx_->at(ctx_->now() + kPressFlash, [this] {
    flash_timer_.reset();
    ctx_->invalidate();
  });
  ctx_->invalidate();
}

ActionFeedback::~ActionFeedback() {
  if (token_) token_->store(true);  // a sequence in flight stops at its next step
}

std::optional<CancelToken> ActionFeedback::begin() {
  if (token_) return std::nullopt;
  token_ = std::make_shared<std::atomic<bool>>(false);
  pressed();
  timeout_timer_ = ctx_->at(ctx_->now() + kActionTimeout, [this] {
    timeout_timer_.reset();
    token_->store(true);
    token_.reset();  // its late result is ignored
    log_warn("{}: the action did not finish within {} s; the key takes presses again", log_name_,
             kActionTimeout.count() / 1000);
    failed();
  });
  return token_;
}

bool ActionFeedback::end(const CancelToken& token) {
  if (!token || token != token_) return false;
  if (timeout_timer_) ctx_->cancel(*timeout_timer_);
  timeout_timer_.reset();
  token_.reset();
  return true;
}

void ActionFeedback::failed() {
  if (fail_timer_) ctx_->cancel(*fail_timer_);
  fail_timer_ = ctx_->at(ctx_->now() + kFailureShown, [this] {
    fail_timer_.reset();
    ctx_->invalidate();
  });
  ctx_->invalidate();
}

}  // namespace ld
