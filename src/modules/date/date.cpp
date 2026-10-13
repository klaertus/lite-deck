#include "modules/date/date.h"
#include <algorithm>
#include <chrono>
#include "modules/common/tap_action.h"
#include "modules/common/state_settings.h"
#include "modules/common/key_look.h"
#include "modules/common/wall_clock.h"
#include "modules/key_module.h"
#include "modules/time_zone.h"

namespace ld {

DateText date_text(const std::tm& t) {
  static const wchar_t* days[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
  static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
  return DateText{days[t.tm_wday % 7], std::to_wstring(t.tm_mday), months[t.tm_mon % 12]};
}

Duration until_next_midnight(const std::tm& t, MakeTime make_time) {
  if (!make_time) make_time = [](std::tm* x) { return std::mktime(x); };
  std::tm now = t;
  now.tm_isdst = -1;
  std::tm next = t;
  next.tm_mday += 1;  // mktime normalizes the end of the month
  next.tm_hour = next.tm_min = next.tm_sec = 0;
  next.tm_isdst = -1;
  const std::time_t a = make_time(&now);
  const std::time_t b = make_time(&next);
  if (a == static_cast<std::time_t>(-1) || b == static_cast<std::time_t>(-1) || b <= a) {
    const long long elapsed = t.tm_hour * 3600LL + t.tm_min * 60LL + t.tm_sec;  // fallback: a 24-hour day
    return Duration((86400LL - elapsed) * 1000LL);
  }
  return Duration(static_cast<long long>(b - a) * 1000LL);
}

namespace {

std::tm local_now() {
  refresh_time_zone();
  std::time_t tt = std::chrono::system_clock::to_time_t(wall_now());
  std::tm tm{};
  localtime_s(&tm, &tt);
  return tm;
}

constexpr ElementSpec kElements[] = {
    {.key = "weekday", .label = "Weekday", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
    {.key = "day", .label = "Day", .size = 36, .weight = 700, .min_size = 10, .max_size = 80, .row = Row::Middle,
     .color = Rgb{255, 255, 255}, .role = TextRole::Value},
    {.key = "month", .label = "Month", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Bottom,
     .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
};

class DateModule final : public TapKey {
public:
  DateModule() : TapKey(kElements) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    shown_ = day_of(local_now());
    schedule();
  }
  std::vector<KeyElement> content() override {
    DateText d = date_text(local_now());
    return {KeyElement{.key = "weekday", .text = d.weekday}, KeyElement{.key = "day", .text = d.day},
            KeyElement{.key = "month", .text = d.month}};
  }

private:
  static int day_of(const std::tm& t) { return (t.tm_year + 1900) * 1000 + t.tm_yday; }
  // Just after the next midnight, kDateMaxSleep at most: a wall-clock jump or a time-zone change shows
  // within 10 min; the key redraws only when the day changed.
  void schedule() {
    const Duration wait = std::min<Duration>(until_next_midnight(local_now()) + Duration(2000), kDateMaxSleep);
    ctx_->at(ctx_->now() + wait, [this] {
      const int day = day_of(local_now());
      if (day != shown_) {
        shown_ = day;
        ctx_->invalidate();
      }
      schedule();
    });
  }
  ModuleContext* ctx_ = nullptr;
  int shown_ = 0;  // the local day on display
};

}  // namespace

ModuleInfo date_module_info() {
  ModuleInfo m;
  m.id = "date";
  m.name = "Date";
  m.description = "Weekday, day and month.";
  m.category = "Time";
  m.hue = "";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"({"fields":[]})");
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("none"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return std::make_unique<DateModule>(); };
  return m;
}

}  // namespace ld
