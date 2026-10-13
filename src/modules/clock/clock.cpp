#include "modules/clock/clock.h"
#include <format>
#include "modules/common/key_look.h"
#include "modules/common/tap_action.h"
#include "modules/common/state_settings.h"
#include "modules/key_module.h"
#include "modules/common/wall_clock.h"
#include "modules/time_zone.h"

namespace ld {

std::wstring format_clock(const std::tm& t, bool seconds) {
  return seconds ? std::format(L"{:02}:{:02}:{:02}", t.tm_hour, t.tm_min, t.tm_sec)
                 : std::format(L"{:02}:{:02}", t.tm_hour, t.tm_min);
}

Duration until_next_second(std::chrono::system_clock::time_point now) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  return Duration(1000 - ms);
}

Duration until_next_minute(std::chrono::system_clock::time_point now) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 60000;
  return Duration(60000 - ms);
}

namespace {

constexpr ElementSpec kTime{.key = "time", .label = "Time", .size = 28, .weight = 700, .min_size = 10, .max_size = 60,
                            .row = Row::Middle, .color = Rgb{255, 255, 255}, .role = TextRole::LongValue};
constexpr ElementSpec kElements[] = {kTime, hours_minutes_spec(kTime), seconds_spec(kTime)};

class ClockModule final : public TapKey {
public:
  ClockModule() : TapKey(kElements) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    seconds_ = s.at("seconds").get<bool>();
    below_ = seconds_ && s.at("seconds_below").get<bool>();  // nothing below without seconds
    schedule();
  }
  std::vector<KeyElement> content() override {
    refresh_time_zone();
    std::time_t tt = std::chrono::system_clock::to_time_t(wall_now());
    std::tm tm{};
    localtime_s(&tm, &tt);
    if (below_)
      return {KeyElement{.key = "hours_minutes", .text = format_clock(tm, false)},
              KeyElement{.key = "seconds", .text = std::format(L"{:02}", tm.tm_sec)}};
    return {KeyElement{.key = "time", .text = format_clock(tm, seconds_)}};
  }

private:
  void schedule() {
    // +2 ms so the tick lands just after the boundary.
    auto now = wall_now();
    auto wait = seconds_ ? until_next_second(now) : until_next_minute(now);
    ctx_->at(ctx_->now() + wait + Duration(2), [this] {
      ctx_->invalidate();
      schedule();
    });
  }
  ModuleContext* ctx_ = nullptr;
  bool seconds_ = true;
  bool below_ = false;
};

}  // namespace

ModuleInfo clock_module_info() {
  ModuleInfo m;
  m.id = "clock";
  m.name = "Clock";
  m.description = "Local time, with or without seconds.";
  m.category = "Time";
  m.hue = "";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"json({"fields":[
    {"key":"seconds","type":"bool","default":true,"label":"Show seconds"},
    {"key":"seconds_below","type":"bool","default":false,"label":"Seconds on a line below",
     "hint":"Hours and minutes large, the seconds smaller underneath.","show_if":{"field":"seconds","equals":true}}]})json");
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("open", "ms-clock:"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return std::make_unique<ClockModule>(); };
  return m;
}

}  // namespace ld
