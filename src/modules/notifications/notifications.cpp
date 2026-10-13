// src/modules/notifications/notifications.cpp
#include "modules/notifications/notifications.h"
#include <algorithm>
#include <utility>
#include "core/log.h"
#include "modules/common/fit.h"
#include "modules/common/marquee.h"

namespace ld {
namespace {

Rgb blend(Rgb fg, Rgb bg, float a) {
  auto mix = [a](std::uint8_t f, std::uint8_t b) {
    return static_cast<std::uint8_t>(static_cast<float>(f) * a + static_cast<float>(b) * (1 - a));
  };
  return Rgb{mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b)};
}

std::wstring collapse(const std::wstring& s) {
  std::wstring out;
  bool space = false;
  for (wchar_t c : s) {
    if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(L' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

constexpr Rgb kAccent{90, 200, 250};
constexpr Rgb kAmber{255, 150, 0};

constexpr Duration kFlash{2000};  // a new notification lights the zone up, fading out over this time

// Rows and alignment do not apply to the strip. The access message is
// Stale but keeps the text colour unless the user picks "state": the strip's default look does not change.
constexpr ElementSpec kStatus{.key = "status", .label = "Access message", .size = 24, .weight = 700, .min_size = 12,
                              .max_size = 48, .keeps_color = true};
constexpr ElementSpec kEmpty{.key = "empty", .label = "\"No notifications\"", .size = 22.5f, .weight = 600,
                             .min_size = 8, .max_size = 60, .color = Rgb{140, 140, 140}};  // same style as "Nothing playing"
constexpr ElementSpec kCount{.key = "count", .label = "Counter", .size = 22.5f, .weight = 600, .min_size = 8,
                             .max_size = 40, .color = Rgb{0, 0, 0}};
constexpr ElementSpec kApp{.key = "app", .label = "App name", .size = 24.5f, .weight = 600, .min_size = 8,
                           .max_size = 48, .color = kAccent};
constexpr ElementSpec kAge{.key = "age", .label = "Age", .size = 19, .weight = 400, .min_size = 8, .max_size = 40};
constexpr ElementSpec kMessage{.key = "message", .label = "Message", .size = 26, .weight = 600, .min_size = 8,
                               .max_size = 60};

// "just now", "5 min", "2 h", "3 d".
std::wstring age_label(Duration age) {
  auto m = std::chrono::duration_cast<std::chrono::minutes>(age).count();
  if (m < 1) return L"just now";
  if (m < 60) return std::to_wstring(m) + L" min";
  if (m < 24 * 60) return std::to_wstring(m / 60) + L" h";
  return std::to_wstring(m / (24 * 60)) + L" d";
}

class NotificationsModule final : public Module {
public:
  explicit NotificationsModule(std::function<std::unique_ptr<NotificationSource>()> f) : factory_(std::move(f)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    ctx_ = &ctx;
    speed_ = s.at("scroll_speed").get<double>();
    text_ = parse_color(s.at("text_color").get<std::string>()).value_or(Rgb{255, 255, 255});
    bg_ = parse_color(s.at("bg_color").get<std::string>()).value_or(Rgb{0, 0, 0});
    source_ = factory_();
    Poster p = ctx.poster();
    source_->request_access([p, this](NotifAccess a) { p.post([this, a] { set_access(a); }); });
  }
  void on_event(const InputEvent& e) override {
    if (e.kind != EventKind::Tap) return;
    auto cur = store_.current();
    if (!cur) return;
    store_.remove(cur->id);
    source_->remove(cur->id);
    current_changed();
  }
  void render(Canvas& c) override {
    const float flash = flash_level();
    const Rgb bg = flash > 0 ? blend(kAmber, bg_, flash * 0.35f) : bg_;
    c.clear(bg);
    if (access_ != NotifAccess::Allowed) {
      overflow_ = false;
      const ResolvedStyle rs = c.resolve(kStatus, ElementInput{.state = ElementState::Stale, .color = text_});
      if (!rs.visible) return;
      TextStyle st = c.style(kStatus, rs);
      const wchar_t* msg =
          access_ == NotifAccess::Denied ? L"⚠ Notifications: access denied" : L"⚠ Notifications unavailable";
      fit_text(c, msg, RectF{8, 0, c.width() - 16, c.height()}, st, kStatus.min_size);
      return;
    }
    // Layout: app name (accent) + age, count pill on the right, then the message below.
    const float h = c.height();
    auto cur = store_.current();
    if (!cur) {
      overflow_ = false;
      const ResolvedStyle rs = c.resolve(kEmpty, ElementInput{.state = ElementState::Idle, .size = h * 0.2f});
      if (!rs.visible) return;
      TextStyle st = c.style(kEmpty, rs);  // the idle colour, shared with the now-playing zone
      st.align = HAlign::Center;  // centred in the zone, both ways
      st.valign = VAlign::Center;
      fit_text(c, L"No notifications", RectF{12, 0, c.width() - 24, h}, st, kEmpty.min_size);
      return;
    }
    const float x = 18, right = c.width() - 16;
    const ResolvedStyle pill_rs = c.resolve(kCount, ElementInput{.size = h * 0.2f});
    RectF pill_r{right + 10, h * 0.32f, 0, 0};  // hidden: no room taken
    if (pill_rs.visible) {
      TextStyle pill = c.style(kCount, pill_rs);
      pill.size *= 1 + 0.25f * flash;  // the pill swells with the flash
      std::wstring count = store_.count() > 99 ? std::wstring(L"99+") : std::to_wstring(store_.count());
      shrink_to_fit(c, count, pill, c.width() * 0.25f, kCount.min_size);  // never crowds out the app name
      float pill_h = pill.size * 1.4f;
      float pill_w = std::max(c.measure(count, pill) + pill.size * 0.9f, pill.size * 1.6f);
      pill_r = RectF{right - pill_w, h * 0.32f - pill_h / 2, pill_w, pill_h};
      c.fill_round_rect(pill_r, pill_h / 2, kAmber);
      c.text(count, RectF{pill_r.x, pill_r.y + 1, pill_r.w, pill_r.h}, pill);
    }

    const MarqueeOptions still{bg};
    const ResolvedStyle app_rs = c.resolve(kApp, ElementInput{.color = kAccent, .size = h * 0.22f});
    TextStyle app = c.style(kApp, app_rs);
    app.align = HAlign::Left;
    const ResolvedStyle age_rs = c.resolve(kAge, ElementInput{.color = blend(text_, bg_, 0.55f), .size = h * 0.17f});
    TextStyle age = c.style(kAge, age_rs);
    age.align = HAlign::Left;
    age_label_ = age_label(std::chrono::duration_cast<Duration>(ctx_->now() - cur->created));
    const std::wstring age_text = age_rs.visible ? L" · " + age_label_ : std::wstring();
    if (!age_text.empty()) shrink_to_fit(c, age_text, age, (right - x) * 0.4f, kAge.min_size);
    const float age_w = age_text.empty() ? 0.f : c.measure(age_text, age);
    const float app_room = std::max(0.f, pill_r.x - 10 - x - age_w);
    const float app_w = app_rs.visible ? std::min(c.measure(collapse(cur->app), app), app_room) : 0.f;
    const float ly = h * 0.32f;
    if (app_rs.visible)
      draw_marquee(c, collapse(cur->app), RectF{x, ly - app.size * 0.75f, app_room, app.size * 1.5f}, app, 0, still);
    if (!age_text.empty()) c.text(age_text, RectF{x + app_w, ly - app.size * 0.75f, age_w + 2, app.size * 1.5f}, age);

    const ResolvedStyle msg_rs = c.resolve(kMessage, ElementInput{.color = text_, .size = h * 0.23f});
    if (!msg_rs.visible) {
      overflow_ = false;
      slow_ = false;
      return;
    }
    TextStyle msg = c.style(kMessage, msg_rs);
    msg.align = HAlign::Left;
    std::wstring t = collapse(cur->title), b = collapse(cur->body);
    std::wstring content = t.empty() ? b : b.empty() ? t : t + L" - " + b;
    double off = scroll_offset(std::chrono::duration_cast<Duration>(ctx_->now() - since_), speed_);
    overflow_ = draw_marquee(c, content, RectF{x, h * 0.68f - msg.size * 0.75f, right - x, msg.size * 1.5f}, msg, off,
                             MarqueeOptions{bg});
    slow_ = overflow_ && scroll_is_slow(off, c.measure(content, msg), marquee_gap(msg));
  }
  bool animating() const override { return overflow_ || flash_level() > 0; }
  bool slow_animation() const override { return slow_ && flash_level() <= 0; }

private:
  static const char* access_name(NotifAccess a) {
    return a == NotifAccess::Allowed ? "granted" : a == NotifAccess::Denied ? "denied" : "unavailable";
  }
  // Granted: polls the list every second. Otherwise: shows the status, forgets the list (it would go
  // stale) and checks the access again every kAccessRecheck, so a grant made later in Settings is seen.
  void set_access(NotifAccess a) {
    const bool first = !access_known_;
    if (!first && a == access_) return;
    if (!first) log_info("notifications: access {}", access_name(a));
    access_known_ = true;
    access_ = a;
    if (a == NotifAccess::Allowed) {
      if (recheck_timer_) ctx_->cancel(std::exchange(recheck_timer_, 0));
      poll();
      poll_timer_ = ctx_->every(kNotificationPoll, [this] { poll(); });
    } else {
      if (poll_timer_) ctx_->cancel(std::exchange(poll_timer_, 0));
      busy_ = false;
      ++fetch_id_;  // an answer still in flight is dropped
      store_ = NotificationStore{};
      synced_ = false;  // those present when access comes back do not flash
      flash_start_.reset();
      overflow_ = slow_ = false;
      if (!recheck_timer_) recheck_timer_ = ctx_->every(kAccessRecheck, [this] { check_access(); });
    }
    ctx_->invalidate();
  }
  // One check in flight at a time; one that never answers is given up after kFetchTimeout.
  void check_access() {
    if (checking_ && ctx_->now() - check_start_ <= kFetchTimeout) return;
    checking_ = true;
    check_start_ = ctx_->now();
    last_check_ = ctx_->now();
    const std::uint64_t id = ++check_id_;
    Poster p = ctx_->poster();
    source_->check_access([p, this, id](NotifAccess a) {
      p.post([this, id, a] {
        if (id != check_id_) return;
        checking_ = false;
        set_access(a);
      });
    });
  }
  void poll() {
    if (busy_) {
      if (ctx_->now() - fetch_start_ <= kFetchTimeout) return;
      log_warn("notifications: fetch did not answer, retrying");
    }
    busy_ = true;
    fetch_start_ = ctx_->now();
    std::uint64_t id = ++fetch_id_;
    Poster p = ctx_->poster();
    source_->fetch([p, this, id](std::optional<std::vector<IncomingNotification>> list) {
      p.post([this, id, list = std::move(list)] { on_list(id, list); });
    });
  }
  void on_list(std::uint64_t id, const std::optional<std::vector<IncomingNotification>>& list) {
    if (id != fetch_id_) return;  // late answer of an abandoned fetch
    busy_ = false;
    if (!list) {
      // Failed: access may have been revoked in Settings. Checked at most every kFailedFetchRecheck.
      if (!last_check_ || ctx_->now() - *last_check_ >= kFailedFetchRecheck) check_access();
      return;
    }
    auto before = store_.current();
    std::size_t count_before = store_.count();
    auto added = store_.sync(*list, ctx_->now());
    auto after = store_.current();
    bool changed_current = (before.has_value() != after.has_value()) || (before && after && before->id != after->id);
    // Flash only for a notification that arrives while running, not for those already there at start.
    if (synced_ && after && std::any_of(added.begin(), added.end(), [&](const auto& a) { return a.id == after->id; }))
      flash_start_ = ctx_->now();
    synced_ = true;
    if (changed_current) current_changed();
    else if (store_.count() != count_before) ctx_->invalidate();
    else if (after && age_label(std::chrono::duration_cast<Duration>(ctx_->now() - after->created)) != age_label_)
      ctx_->invalidate();
  }
  void current_changed() {
    since_ = ctx_->now();
    ctx_->invalidate();
  }
  std::function<std::unique_ptr<NotificationSource>()> factory_;
  std::unique_ptr<NotificationSource> source_;
  ModuleContext* ctx_ = nullptr;
  NotificationStore store_;
  NotifAccess access_ = NotifAccess::Unavailable;
  bool access_known_ = false;  // the first answer is not logged as a change
  TimerId poll_timer_ = 0, recheck_timer_ = 0;
  bool checking_ = false;
  std::uint64_t check_id_ = 0;
  TimePoint check_start_{};
  std::optional<TimePoint> last_check_;
  bool busy_ = false;
  std::uint64_t fetch_id_ = 0;
  TimePoint fetch_start_{};
  static constexpr Duration kFetchTimeout{10000};
  // 1 right after a new notification, fading to 0 over kFlash.
  float flash_level() const {
    if (!flash_start_ || !ctx_) return 0;
    auto t = ctx_->now() - *flash_start_;
    if (t >= kFlash) return 0;
    return 1.f - static_cast<float>(std::chrono::duration_cast<Duration>(t).count()) / static_cast<float>(kFlash.count());
  }
  bool overflow_ = false;
  bool slow_ = false;
  bool synced_ = false;
  std::optional<TimePoint> flash_start_;
  std::wstring age_label_;
  double speed_ = 2;
  Rgb text_{255, 255, 255}, bg_{0, 0, 0};
  TimePoint since_{};
};

}  // namespace

std::unique_ptr<Module> make_notifications_module(std::function<std::unique_ptr<NotificationSource>()> factory) {
  return std::make_unique<NotificationsModule>(std::move(factory));
}

ModuleInfo notifications_module_info() {
  ModuleInfo m;
  m.id = "notifications";
  m.name = "Notifications";
  m.description = "Windows notifications, one at a time. Tap to dismiss.";
  m.category = "System";
  m.hue = "#5ac8fa";
  m.slots = {SlotKind::StripZone};
  m.settings_schema = nlohmann::json::parse(R"({"fields":[
    {"key":"scroll_speed","type":"number","default":2,"min":0.5,"max":10,"label":"Scroll speed"},
    {"key":"text_color","type":"color","default":"#ffffff","label":"Text color"},
    {"key":"bg_color","type":"color","default":"#000000","label":"Background color"}]})");
  m.elements = {kStatus, kEmpty, kCount, kApp, kAge, kMessage};
  m.create = [] { return make_notifications_module(make_winrt_notification_source); };
  return m;
}

}  // namespace ld
