// src/modules/sun/sun.cpp
#include "modules/sun/sun.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include "core/log.h"
#include "core/strings.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/wall_clock.h"
#include "modules/key_module.h"
#include "modules/time_zone.h"
#include "modules/weather/weather.h"

namespace ld {
using nlohmann::json;

LocalZone system_zone() {
  LocalZone z;
  z.to_local = [](std::int64_t unix_s) {
    refresh_time_zone();
    std::tm out{};
    const std::time_t t = static_cast<std::time_t>(unix_s);
    localtime_s(&out, &t);
    return out;
  };
  z.midnight = [](CivilDate d) {
    refresh_time_zone();
    std::tm t{};
    t.tm_year = d.year - 1900;
    t.tm_mon = d.month - 1;
    t.tm_mday = d.day;
    t.tm_isdst = -1;  // Windows decides: 23-hour and 25-hour days come out right
    return static_cast<std::int64_t>(std::mktime(&t));
  };
  return z;
}

namespace {

constexpr Rgb kNext{0xff, 0xd2, 0x3f};      // the next event
constexpr Rgb kOnlineDot{0x5a, 0xc8, 0xfa};  // the info blue of the strip's app name
constexpr ElementSpec kElements[] = {
    {.key = "status", .label = "Status", .size = 26, .weight = 700, .min_size = 10, .max_size = 40, .row = Row::Middle,
     .color = kNext, .role = TextRole::Secondary},
    // "City?" when no city is configured: same look as the weather key (small, always idle).
    {.key = "city_hint", .label = "City missing", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
     .row = Row::Middle, .role = TextRole::Secondary},
    {.key = "symbol1", .label = "Sunrise symbol", .size = 17, .weight = 600, .min_size = 10, .max_size = 40,
     .row = Row::Line1, .align = HAlign::Left, .color = kNext, .role = TextRole::Symbol},
    {.key = "line1", .label = "Sunrise", .size = 26, .weight = 700, .min_size = 10, .max_size = 40, .row = Row::Line1,
     .align = HAlign::Right, .color = kNext, .role = TextRole::TwoLine},
    {.key = "symbol2", .label = "Sunset symbol", .size = 17, .weight = 600, .min_size = 10, .max_size = 40,
     .row = Row::Line2, .align = HAlign::Left, .color = kNext, .role = TextRole::Symbol},
    {.key = "line2", .label = "Sunset", .size = 26, .weight = 700, .min_size = 10, .max_size = 40, .row = Row::Line2,
     .align = HAlign::Right, .color = kNext, .role = TextRole::TwoLine},
    // Plain arrows and a plain dot: "☀" would fall back to the colour emoji under the COLR renderer.
    {.key = "online", .label = "Online times dot", .size = 10, .weight = 400, .min_size = 8, .max_size = 20,
     .row = Row::Top, .align = HAlign::Right, .color = kOnlineDot, .keeps_color = true, .role = TextRole::Symbol},
};

std::int64_t wall_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(wall_now().time_since_epoch()).count();
}

std::string trimmed(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string describe(const HttpResponse& r) { return r.error.empty() ? std::format("HTTP {}", r.status) : r.error; }

// What the key shows: the same value means no invalidation.
struct Shown {
  SunKind kind = SunKind::Normal;
  SunNext next = SunNext::Rise;
  std::wstring rise, set;
  bool online = false;
  bool operator==(const Shown&) const = default;
};

class SunModule final : public KeyModule {
public:
  explicit SunModule(LocalZone zone) : KeyModule(kElements), zone_(std::move(zone)) {}

  void start(ModuleContext& ctx, const json& s) override {
    ctx_ = &ctx;
    city_ = trimmed(s.at("city").get<std::string>());  // settings always went through apply_schema
    verify_days_ = s.at("verify_days").get<int>();
    if (!zone_.to_local || !zone_.midnight) zone_ = system_zone();
    unknown_city_ = city_.empty();
    load(ctx.load_state());
    tick();
  }

  std::vector<KeyElement> content() override {
    if (unknown_city_) return {KeyElement{.key = "city_hint", .text = L"City?", .state = ElementState::Idle}};
    if (!shown_) return {KeyElement{.key = "status", .text = kDash, .state = ElementState::Idle}};
    std::vector<KeyElement> out;
    if (shown_->kind == SunKind::PolarDay) {
      out.push_back(KeyElement{.key = "status", .text = L"Polar day"});
    } else if (shown_->kind == SunKind::PolarNight) {
      out.push_back(KeyElement{.key = "status", .text = L"Polar night", .state = ElementState::Idle});
    } else {
      const ElementState rise = shown_->next == SunNext::Rise ? ElementState::Normal : ElementState::Idle;
      const ElementState set = shown_->next == SunNext::Set ? ElementState::Normal : ElementState::Idle;
      out.push_back(KeyElement{.key = "symbol1", .text = L"↑", .state = rise});
      out.push_back(KeyElement{.key = "line1", .text = shown_->rise, .state = rise});
      out.push_back(KeyElement{.key = "symbol2", .text = L"↓", .state = set});
      out.push_back(KeyElement{.key = "line2", .text = shown_->set, .state = set});
    }
    if (shown_->online) out.push_back(KeyElement{.key = "online", .text = L"●"});
    return out;
  }

private:
  // state.json: {"city", "lat", "lon", "timezone", "verified_on", "correction": {"rise_s", "set_s"}}.
  // The state of another city is ignored: geocode again, check again.
  void load(const json& st) {
    if (unknown_city_ || !st.is_object()) return;
    const auto city = st.find("city");
    if (city == st.end() || !city->is_string() || city->get<std::string>() != city_) return;
    const auto lat = st.find("lat");
    const auto lon = st.find("lon");
    if (lat == st.end() || !lat->is_number() || lon == st.end() || !lon->is_number()) return;
    point_ = GeoPoint{lat->get<double>(), lon->get<double>(), ""};
    if (const auto tz = st.find("timezone"); tz != st.end() && tz->is_string()) point_->timezone = tz->get<std::string>();
    if (const auto v = st.find("verified_on"); v != st.end() && v->is_string())
      if (const auto d = parse_iso_date(v->get<std::string>())) verified_day_ = days_from_civil(*d);
    if (const auto c = st.find("correction"); c != st.end() && c->is_object()) {
      const auto r = c->find("rise_s");
      const auto s = c->find("set_s");
      if (r != c->end() && r->is_number_integer() && s != c->end() && s->is_number_integer()) {
        const SunCorrection loaded{r->get<std::int64_t>(), s->get<std::int64_t>()};
        // A real correction is never larger than kSunMaxGap: a bigger one is corrupt and could keep
        // the next change in the past (a wake-up every second).
        if (std::llabs(loaded.rise_s) <= kSunMaxGap && std::llabs(loaded.set_s) <= kSunMaxGap) correction_ = loaded;
      }
    }
  }
  void save() {
    json st{{"city", city_}, {"lat", point_->lat}, {"lon", point_->lon}, {"timezone", point_->timezone}};
    if (verified_day_) st["verified_on"] = format_iso_date(civil_from_days(*verified_day_));
    if (correction_) st["correction"] = json{{"rise_s", correction_->rise_s}, {"set_s", correction_->set_s}};
    ctx_->save_state(st);
  }

  // Every wake-up: geocode if needed (a failed geocoding is retried here, every 10 min), recompute
  // what the key shows, check online when due, then sleep until just after the next event or
  // midnight, 10 min at most.
  void tick() {
    if (unknown_city_) return;
    const std::int64_t now = wall_seconds();
    if (!point_) {
      geocode();
    } else {
      update_view(now);
      if (check_due(now)) check();
    }
    schedule(now);
  }
  void schedule(std::int64_t now) {
    if (timer_) ctx_->cancel(*timer_);
    std::int64_t wait = kSunMaxSleepS;
    if (shown_) wait = std::clamp<std::int64_t>(change_at_ + 1 - now, 1, kSunMaxSleepS);
    timer_ = ctx_->at(ctx_->now() + Duration(wait * 1000), [this] {
      timer_.reset();
      tick();
    });
  }
  void update_view(std::int64_t now) {
    const SunView v = sun_view(now, point_->lat, point_->lon, zone_, correction_.value_or(SunCorrection{}));
    Shown next;
    next.kind = v.kind;
    next.next = v.next;
    if (v.kind == SunKind::Normal) {
      next.rise = format_hm(v.rise, zone_);
      next.set = format_hm(v.set, zone_);
    }
    next.online = correction_.has_value();
    change_at_ = v.change_at;
    if (shown_ != next) {
      shown_ = std::move(next);
      ctx_->invalidate();
    }
  }

  void geocode() {
    if (busy_) return;
    busy_ = true;
    ctx_->http_get(geocode_url(city_), [this](const HttpResponse& r) { on_geocode(r); });
  }
  void on_geocode(const HttpResponse& r) {
    busy_ = false;
    if (!r.ok()) {  // network or server: retried at the next wake-up, 10 min later, until it works
      if (!geocode_failing_) log_info("sun: geocoding {} failed ({}), retrying every 10 min", city_, describe(r));
      geocode_failing_ = true;
      return;
    }
    GeocodeResult g = read_geocode(r.body);
    if (g.status == GeocodeStatus::Unreadable) {  // captive portal, error page: transient, like a network error
      if (!geocode_failing_) log_info("sun: unreadable geocoding answer for {}, retrying every 10 min", city_);
      geocode_failing_ = true;
      return;
    }
    geocode_failing_ = false;
    std::optional<GeoPoint> p = std::move(g.point);
    if (!p) {
      log_warn("sun: city not found: {}", city_);
      unknown_city_ = true;
      if (timer_) ctx_->cancel(*timer_);
      timer_.reset();
      ctx_->invalidate();
      return;
    }
    point_ = std::move(p);
    verified_day_.reset();
    correction_.reset();
    log_info("sun: {} -> {:.4f}, {:.4f} ({}); times are shown in this PC's time zone", city_, point_->lat,
             point_->lon, point_->timezone.empty() ? std::string("time zone unknown") : point_->timezone);
    save();
    tick();
  }

  std::int64_t local_day(std::int64_t now) const {
    const std::tm t = zone_.to_local(now);
    return days_from_civil({t.tm_year + 1900, t.tm_mon + 1, t.tm_mday});
  }
  bool check_due(std::int64_t now) const {
    if (verify_days_ <= 0 || checking_) return false;
    const std::int64_t today = local_day(now);
    if (failed_day_ == today) return false;  // after a failure, the next try is the next day
    // llabs: a date "in the future" (written while the clock was wrong) must not block the checks.
    return !verified_day_ || std::llabs(today - *verified_day_) >= verify_days_;
  }
  void check() {
    checking_ = true;
    ctx_->http_get(sun_check_url(point_->lat, point_->lon), [this](const HttpResponse& r) { on_check(r); });
  }
  // A failed or unusable check changes nothing (display, correction, state) and is retried tomorrow.
  void check_failed(std::int64_t today, const std::string& why) {
    failed_day_ = today;
    log_info("sun: online check {}, keeping the local times; next try tomorrow", why);
  }
  void on_check(const HttpResponse& r) {
    checking_ = false;
    const std::int64_t today = local_day(wall_seconds());
    const std::optional<OnlineSun> online = r.ok() ? parse_online_sun(r.body) : std::nullopt;
    if (!online) {  // the display never waits for the network
      check_failed(today, std::format("failed ({})", r.ok() ? std::string("unreadable answer") : describe(r)));
      return;
    }
    const SunTimes local = sun_times(online->date, point_->lat, point_->lon);
    if (local.kind != SunKind::Normal) {  // nothing to compare, ever: the check is done
      failed_day_.reset();
      verified_day_ = today;
      correction_.reset();
      log_info("sun: polar day or night, nothing to compare online");
    } else if (const SunCheck c = compare_sun(local, *online); !c.usable) {
      // Typically a wrong utc_offset_seconds on a DST-switch day: a gap no algorithm error explains.
      check_failed(today, std::format("unusable (Open-Meteo {} and {} vs local {} and {}, gap above {} s)",
                                      narrow(format_hm(online->rise, zone_)), narrow(format_hm(online->set, zone_)),
                                      narrow(format_hm(local.rise, zone_)), narrow(format_hm(local.set, zone_)),
                                      kSunMaxGap));
      return;
    } else if (c.agree) {
      failed_day_.reset();
      verified_day_ = today;
      correction_.reset();
      log_info("sun: online check agrees (sunrise {:+} s, sunset {:+} s)", c.correction.rise_s, c.correction.set_s);
    } else {
      failed_day_.reset();
      verified_day_ = today;
      correction_ = c.correction;
      log_warn("sun: local sunrise {} and sunset {} differ from Open-Meteo's {} and {} by more than 2 min; "
               "showing the online times until the next check",
               narrow(format_hm(local.rise, zone_)), narrow(format_hm(local.set, zone_)),
               narrow(format_hm(online->rise, zone_)), narrow(format_hm(online->set, zone_)));
    }
    save();
    tick();
  }

  LocalZone zone_;
  ModuleContext* ctx_ = nullptr;
  std::string city_;
  int verify_days_ = 7;
  std::optional<GeoPoint> point_;
  std::optional<std::int64_t> verified_day_;  // local day number of the last successful check
  std::optional<std::int64_t> failed_day_;    // local day of the last failed or unusable check
  std::optional<SunCorrection> correction_;   // online - local, until the next check
  std::optional<Shown> shown_;
  std::int64_t change_at_ = 0;
  std::optional<TimerId> timer_;
  bool unknown_city_ = false;
  bool busy_ = false;      // geocoding in flight
  bool checking_ = false;  // online check in flight
  bool geocode_failing_ = false;
};

}  // namespace

std::unique_ptr<Module> make_sun_module(LocalZone zone) { return std::make_unique<SunModule>(std::move(zone)); }

ModuleInfo sun_module_info() {
  ModuleInfo m;
  m.id = "sun";
  m.name = "Sunrise / sunset";
  m.description = "Next sunrise and sunset for a city.";
  m.category = "Weather & sky";
  m.hue = "#ffd23f";
  m.slots = {SlotKind::Key};
  m.settings_schema = json::parse(R"J({"fields":[
    {"key":"city","type":"string","default":"","label":"City","placeholder":"Brussels"},
    {"key":"verify_days","type":"int","default":7,"min":0,"max":30,"label":"Check online every (days)","hint":"0 = never."}]})J");
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_sun_module({}); };
  return m;
}

}  // namespace ld
