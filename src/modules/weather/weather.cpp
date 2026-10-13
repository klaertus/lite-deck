// src/modules/weather/weather.cpp
#include "modules/common/tap_action.h"
#include "modules/weather/weather.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include "core/http.h"
#include "core/log.h"
#include "core/strings.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/common/wall_clock.h"
#include "modules/key_module.h"
#include "modules/time_zone.h"
#include "modules/weather/normals.h"

namespace ld {
using nlohmann::json;

GeocodeResult read_geocode(std::string_view body) {
  const json j = json::parse(body, nullptr, false);
  if (!j.is_object() || j.contains("error")) return {GeocodeStatus::Unreadable, std::nullopt};
  // Open-Meteo leaves "results" out when nothing matches.
  const auto results = j.find("results");
  if (results == j.end() || (results->is_array() && results->empty())) return {GeocodeStatus::NoMatch, std::nullopt};
  if (!results->is_array() || !(*results)[0].is_object()) return {GeocodeStatus::Unreadable, std::nullopt};
  const json& r = (*results)[0];
  const auto lat = r.find("latitude");
  const auto lon = r.find("longitude");
  if (lat == r.end() || !lat->is_number() || lon == r.end() || !lon->is_number())
    return {GeocodeStatus::Unreadable, std::nullopt};
  GeoPoint p{lat->get<double>(), lon->get<double>(), ""};
  if (const auto tz = r.find("timezone"); tz != r.end() && tz->is_string()) p.timezone = tz->get<std::string>();
  return {GeocodeStatus::Found, std::move(p)};
}

std::optional<GeoPoint> parse_geocode(std::string_view body) { return read_geocode(body).point; }

std::optional<Weather> parse_forecast(std::string_view body) {
  json j = json::parse(body, nullptr, false);
  try {
    if (!j.is_object()) return std::nullopt;
    const json& cur = j.at("current");
    const json& daily = j.at("daily");
    return Weather{cur.at("temperature_2m").get<double>(), cur.at("weather_code").get<int>(),
                   daily.at("temperature_2m_min").at(0).get<double>(), daily.at("temperature_2m_max").at(0).get<double>()};
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

std::wstring weather_icon(int c) {
  if (c == 0) return L"☀️";
  if (c <= 2) return L"🌤️";
  if (c == 3) return L"☁️";
  if (c == 45 || c == 48) return L"🌫️";
  if ((c >= 51 && c <= 57) || (c >= 80 && c <= 82)) return L"🌦️";
  if (c >= 61 && c <= 67) return L"🌧️";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return L"❄️";
  if (c >= 95) return L"⛈️";
  return L"🌡️";
}

std::string geocode_url(const std::string& city) {
  return "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + url_encode(city);
}

std::string forecast_url(GeoPoint p) {
  return std::format(
      "https://api.open-meteo.com/v1/forecast?latitude={:.4f}&longitude={:.4f}"
      "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1",
      p.lat, p.lon);
}

namespace {

// What the key shows: the same rendering means no invalidation.
bool same_display(const Weather& a, const Weather& b) {
  return a.code == b.code && std::lround(a.temp) == std::lround(b.temp) && std::lround(a.tmin) == std::lround(b.tmin) &&
         std::lround(a.tmax) == std::lround(b.tmax);
}

constexpr ElementSpec kElements[] = {
    {.key = "icon", .label = "Icon", .size = 26, .weight = 400, .min_size = 10, .max_size = 80, .icon = true,
     .kind = ElementKind::Icon, .row = Row::Top, .color = Rgb{0xff, 0xd2, 0x3f}},
    {.key = "temp", .label = "Temperature", .size = 36, .weight = 700, .min_size = 10, .max_size = 80,
     .row = Row::Middle, .color_mode = ColorMode::Gradient, .color = Rgb{255, 255, 255}, .gradient = true, .role = TextRole::Value},
    {.key = "range", .label = "Min / max", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
     .row = Row::Bottom, .color = kSecondaryGrey, .role = TextRole::Secondary},
    // "City?" when no city is configured: small text, always idle (the idle state colour).
    {.key = "status", .label = "Status", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
     .row = Row::Middle, .role = TextRole::Secondary},
};

// The PC's local date (the normals are per calendar day).
CivilDate local_today() {
  refresh_time_zone();
  const std::time_t tt = std::chrono::system_clock::to_time_t(wall_now());
  std::tm tm{};
  localtime_s(&tm, &tt);
  return CivilDate{tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

class WeatherModule final : public TapKey {
public:
  WeatherModule() : TapKey(kElements) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    city_ = s.at("city").get<std::string>();
    auto b = city_.find_first_not_of(" \t");
    auto e = city_.find_last_not_of(" \t");
    city_ = b == std::string::npos ? std::string() : city_.substr(b, e - b + 1);
    absolute_ = s.value("gradient_basis", std::string("anomaly")) == "absolute";
    anomaly_gradient_ = gradient_from(s, kWeatherAnomalyGradient);
    json st = ctx.load_state();
    const bool same_city = st.is_object() && st.contains("city") && st["city"].is_string() &&
                           st["city"].get<std::string>() == city_;  // value() would throw on a non-string
    if (same_city && st.contains("lat") && st.contains("lon") && st["lat"].is_number() && st["lon"].is_number()) {
      point_ = GeoPoint{st["lat"].get<double>(), st["lon"].get<double>()};
      if (st.contains("normals")) normals_ = normals_from_json(st["normals"]);
    }
    if (city_.empty()) {
      unknown_city_ = true;
      ctx.invalidate();
    }
    refresh();
    ctx.every(kWeatherPeriod, [this] { refresh(); });
  }
  std::vector<KeyElement> content() override {
    if (unknown_city_) return {KeyElement{.key = "status", .text = L"City?", .state = ElementState::Idle}};
    if (!weather_) return {KeyElement{.key = "temp", .text = kDash, .state = ElementState::Idle}};
    const ElementState st = stale_ ? ElementState::Stale : ElementState::Normal;
    KeyElement temp{.key = "temp", .text = std::format(L"{}°", std::lround(weather_->temp)), .state = st};
    if (!absolute_ && normals_) {
      // The gap of today's forecast daily mean to the seasonal normal (itself a daily mean): like with
      // like, so the colour does not follow the time of day.
      temp.metric = (weather_->tmin + weather_->tmax) / 2.0 - normal_for(normals_->values, local_today());
      temp.gradient = anomaly_gradient_;
    } else {  // the temperature itself, also while no normal is known
      temp.metric = weather_->temp;
      temp.gradient = kWeatherAbsoluteGradient;
    }
    return {KeyElement{.key = "icon", .text = weather_icon(weather_->code), .state = st}, std::move(temp),
            KeyElement{.key = "range",
                       .text = std::format(L"{}° / {}°", std::lround(weather_->tmin), std::lround(weather_->tmax)),
                       .state = st}};
  }

private:
  void refresh() {
    if (busy_ || city_.empty()) return;
    busy_ = true;
    if (!point_) {
      ctx_->http_get(geocode_url(city_), [this](const HttpResponse& r) { on_geocode(r); });
    } else {
      ctx_->http_get(forecast_url(*point_), [this](const HttpResponse& r) { on_forecast(r); });
    }
  }
  void save() {
    json st{{"city", city_}, {"lat", point_->lat}, {"lon", point_->lon}};
    if (normals_) st["normals"] = normals_to_json(*normals_);
    ctx_->save_state(st);
  }
  void on_geocode(const HttpResponse& r) {
    busy_ = false;
    if (!r.ok()) {
      mark_stale();
      return;
    }
    GeocodeResult g = read_geocode(r.body);
    if (g.status == GeocodeStatus::Unreadable) {  // not an answer: retried at the next period, like a network error
      log_info("weather: unreadable geocoding answer for {}, retrying in 15 min", city_);
      mark_stale();
      return;
    }
    point_ = std::move(g.point);
    bool unknown = !point_;
    if (unknown != unknown_city_) {
      unknown_city_ = unknown;
      ctx_->invalidate();
    }
    if (!point_) return;
    normals_.reset();  // a new place: its own normals
    save();
    refresh();
  }
  void on_forecast(const HttpResponse& r) {
    busy_ = false;
    auto w = r.ok() ? parse_forecast(r.body) : std::nullopt;
    if (!w) {
      mark_stale();
      return;
    }
    const bool changed = stale_ || !weather_ || !same_display(*weather_, *w);
    weather_ = w;
    stale_ = false;
    if (changed) ctx_->invalidate();
    fetch_normals();
  }
  void mark_stale() {
    if (weather_ && !stale_) {
      stale_ = true;
      ctx_->invalidate();
    }
  }
  // At most once a month while it works; once a day after a failure.
  void fetch_normals() {
    if (absolute_ || normals_busy_ || !point_) return;
    const CivilDate today = local_today();
    const std::int64_t day = days_from_civil(today);
    if (normals_) {
      const std::int64_t age = day - days_from_civil(normals_->fetched);
      if (age >= 0 && age < kNormalsMaxAgeDays) return;  // a date in the future is stale
    }
    if (failed_day_ == day) return;
    normals_busy_ = true;
    ctx_->http_get(normals_url(point_->lat, point_->lon, today.year - years_, today.year - 1),
                   [this](const HttpResponse& r) { on_normals(r); });
  }
  void on_normals(const HttpResponse& r) {
    normals_busy_ = false;
    const CivilDate today = local_today();
    if (r.error == kResponseTooLarge && years_ > kMinNormalYears) {
      years_ = std::max(kMinNormalYears, years_ - 10);
      log_info("weather: the seasonal normals answer is above 1 MB; asking for {} years", years_);
      fetch_normals();
      return;
    }
    const auto days = r.ok() ? parse_daily_means(r.body) : std::nullopt;
    const auto normals = days ? compute_normals(*days) : std::nullopt;
    if (!normals) {
      failed_day_ = days_from_civil(today);
      log_info("weather: seasonal normals unavailable ({}); colouring by the temperature until tomorrow",
               r.error.empty() ? std::format("HTTP {}", r.status) : r.error);
      return;
    }
    failed_day_.reset();
    normals_ = CachedNormals{*normals, today, years_};
    save();
    if (weather_) ctx_->invalidate();
  }
  ModuleContext* ctx_ = nullptr;
  std::string city_;
  bool absolute_ = false;
  Gradient anomaly_gradient_ = kWeatherAnomalyGradient;
  std::optional<GeoPoint> point_;
  std::optional<Weather> weather_;
  std::optional<CachedNormals> normals_;
  std::optional<std::int64_t> failed_day_;  // local day of the last failed normals request
  int years_ = kNormalYears;
  bool normals_busy_ = false;
  bool stale_ = false;
  bool unknown_city_ = false;
  bool busy_ = false;
};

}  // namespace

ModuleInfo weather_module_info() {
  ModuleInfo m;
  m.id = "weather";
  m.name = "Weather";
  m.description = "Temperature, sky and today's min and max for a city.";
  m.category = "Weather & sky";
  m.hue = "#ffd23f";
  m.slots = {SlotKind::Key};
  m.settings_schema = with_fields(nlohmann::json::parse(R"({"fields":[
    {"key":"city","type":"string","default":"","label":"City","placeholder":"Brussels"},
    {"key":"gradient_basis","type":"enum","default":"anomaly","label":"Color by","group":"gradient",
     "options":[{"value":"anomaly","label":"Difference from normal"},{"value":"absolute","label":"Actual temperature"}]}]})"),
                                  gradient_fields("°C", kWeatherAnomalyGradient, -30, 30));
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("open", "https://open-meteo.com"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return std::make_unique<WeatherModule>(); };
  return m;
}

}  // namespace ld
