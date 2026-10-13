// src/modules/http_sensor/http_sensor.cpp
#include "modules/http_sensor/http_sensor.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include "core/http.h"
#include "core/log.h"
#include "core/strings.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/key_module.h"

namespace ld {
using nlohmann::json;

std::optional<std::vector<JsonStep>> parse_json_path(std::string_view p) {
  std::vector<JsonStep> out;
  if (p.starts_with("$.")) p.remove_prefix(2);
  else if (p.starts_with("$")) p.remove_prefix(1);
  std::size_t i = 0;
  bool need_key = false;  // after a dot
  while (i < p.size()) {
    if (p[i] == '[') {
      const std::size_t close = p.find(']', i);
      if (close == std::string_view::npos || close == i + 1) return std::nullopt;
      std::size_t n = 0;
      const auto [end, ec] = std::from_chars(p.data() + i + 1, p.data() + close, n);
      if (ec != std::errc() || end != p.data() + close) return std::nullopt;
      out.push_back(JsonStep{.key = {}, .index = n});
      i = close + 1;
      need_key = false;
      if (i < p.size() && p[i] == '.') {
        need_key = true;
        ++i;
      } else if (i < p.size() && p[i] != '[') {
        return std::nullopt;
      }
      continue;
    }
    const std::size_t end = std::min(p.find_first_of(".[]", i), p.size());
    if (end == i || (end < p.size() && p[end] == ']')) return std::nullopt;
    out.push_back(JsonStep{.key = std::string(p.substr(i, end - i))});
    i = end;
    need_key = false;
    if (i < p.size() && p[i] == '.') {
      need_key = true;
      ++i;
    }
  }
  if (need_key) return std::nullopt;  // a trailing dot
  return out;
}

const json* json_at(const json& root, const std::vector<JsonStep>& path) {
  const json* v = &root;
  for (const JsonStep& s : path) {
    if (!s.key.empty()) {
      if (!v->is_object()) return nullptr;
      const auto it = v->find(s.key);
      if (it == v->end()) return nullptr;
      v = &*it;
    } else {
      if (!v->is_array() || s.index >= v->size()) return nullptr;
      v = &(*v)[s.index];
    }
  }
  return v;
}

namespace {

constexpr Rgb kOrchid{0xcf, 0x7b, 0xff};
constexpr std::size_t kMaxText = 24;           // characters shown of a text value, the ellipsis included
constexpr int kMaxBackoffS = 600;              // the longest wait after failures (unless interval_s is longer)
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10, .max_size = 80, .row = Row::Middle,
     .color_mode = ColorMode::State, .color = kOrchid, .role = TextRole::Value},
    {.key = "unit", .label = "Unit", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Bottom,
     .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
};

std::string trimmed(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  return std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
}

// A string that is a number ("21.5", as Home Assistant states are) counts as one.
std::optional<double> number_of(const json& v) {
  if (v.is_number()) return v.get<double>();
  if (!v.is_string()) return std::nullopt;
  const std::string s = trimmed(v.get<std::string>());
  double d = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
  if (s.empty() || ec != std::errc() || end != s.data() + s.size() || !std::isfinite(d)) return std::nullopt;
  return d;
}

bool usable_url(const std::string& u) {
  const bool http = u.starts_with("http://"), https = u.starts_with("https://");
  if (!http && !https) return false;
  const std::size_t host = http ? 7 : 8;
  if (u.size() <= host || u[host] == '/' || u[host] == '?' || u[host] == '#') return false;
  return std::none_of(u.begin(), u.end(), [](char c) { return static_cast<unsigned char>(c) <= 0x20 || c == 0x7f; });
}

std::wstring cut(std::wstring s) {
  if (s.size() <= kMaxText) return s;
  s.resize(kMaxText - 1);
  return s + L"…";
}

class HttpSensorModule final : public KeyModule {
public:
  HttpSensorModule() : KeyModule(kElements) {}
  void start(ModuleContext& ctx, const json& s) override {
    ctx_ = &ctx;
    url_ = trimmed(s.at("url").get<std::string>());
    label_ = widen(s.at("label").get<std::string>());
    unit_ = widen(s.at("unit").get<std::string>());
    decimals_ = s.at("decimals").get<int>();
    interval_s_ = s.at("interval_s").get<int>();
    header_name_ = trimmed(s.at("header_name").get<std::string>());
    header_value_ = s.at("header_value").get<std::string>();
    const std::string direction = s.at("direction").get<std::string>();
    const double warn = s.at("warn").get<double>(), crit = s.at("crit").get<double>();
    const double h = std::max(0.0, s.at("hysteresis").get<double>());
    if (direction == "above") thresholds_ = Thresholds{warn, std::max(crit, warn), h};
    if (direction == "below") {  // the lower the worse: thresholds on the opposite
      thresholds_ = Thresholds{-warn, -std::min(crit, warn), h};
      below_ = true;
    }
    const auto path = parse_json_path(trimmed(s.at("json_path").get<std::string>()));
    if (path) path_ = *path;
    ctx.invalidate();
    if (!usable_url(url_)) problem_ = L"URL?";
    else if (!path) problem_ = L"Path?";
    else if (!header_name_.empty() && !valid_http_header(header_name_, header_value_)) problem_ = L"Header?";
    if (!problem_.empty()) return;
    fetch();
  }
  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    if (!label_.empty()) out.push_back(KeyElement{.key = "label", .text = label_});
    if (!problem_.empty()) {
      out.push_back(KeyElement{.key = "value", .text = problem_, .state = ElementState::Idle});
      return out;
    }
    if (!text_) {  // nothing received yet
      const ElementState st = failures_ ? ElementState::Stale : ElementState::Idle;
      out.push_back(KeyElement{.key = "value", .text = kDash, .state = st});
      return out;
    }
    if (missing_) {
      out.push_back(KeyElement{.key = "value", .text = L"?", .state = ElementState::Idle});
      return out;
    }
    const ElementState st = failures_ ? ElementState::Stale : state_;
    out.push_back(KeyElement{.key = "value", .text = *text_, .state = st});
    if (!unit_.empty()) out.push_back(KeyElement{.key = "unit", .text = unit_, .state = st});
    return out;
  }

private:
  void fetch() {
    timer_ = 0;
    // A host the user typed may not answer: its own lane, so it never delays the other modules.
    HttpRequest req{.url = url_, .allow_plain_http = url_.starts_with("http://"), .lane = HttpLane::Isolated};
    if (!header_name_.empty()) req.header.emplace(header_name_, header_value_);
    ctx_->http_get(req, [this](const HttpResponse& r) { on_response(r); });
  }
  void on_response(const HttpResponse& r) {
    if (!r.ok()) return failed(r.error.empty() ? std::format("HTTP {}", r.status) : r.error);
    const json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) return failed("the answer is not JSON");
    const bool was_failing = failures_ > 0;
    failures_ = 0;
    const json* v = json_at(j, path_);
    std::optional<std::wstring> text;
    bool missing = false;
    ElementState st = ElementState::Normal;
    if (!v || v->is_object() || v->is_array()) {
      missing = true;
      text = L"?";
    } else if (const auto n = number_of(*v)) {
      double x = *n;
      if (std::fabs(x) < 0.5 * std::pow(10.0, -decimals_)) x = 0;  // no "-0.0"
      text = format_decimal(x, decimals_);
      if (thresholds_) st = threshold_state(state_, below_ ? -*n : *n, *thresholds_);
    } else if (v->is_boolean()) {
      text = v->get<bool>() ? L"true" : L"false";
    } else if (v->is_string()) {
      text = cut(widen(v->get<std::string>()));
    } else {
      text = L"null";
    }
    const bool changed = was_failing || text != text_ || missing != missing_ || st != state_;
    text_ = std::move(text);
    missing_ = missing;
    state_ = st;
    if (changed) ctx_->invalidate();
    schedule(interval_s_);
  }
  void failed(const std::string& why) {
    ++failures_;
    // interval_s, doubled at each failure in a row, at most 10 minutes (or interval_s if longer).
    const long long cap = std::max(kMaxBackoffS, interval_s_);
    const long long wait = std::min<long long>(cap, static_cast<long long>(interval_s_) << std::min(failures_, 20));
    if (failures_ == 1) {
      log_info("http_sensor: {} failed ({}); next try in {} s", url_for_log(url_), why, wait);
      ctx_->invalidate();  // the value turns stale
    }
    schedule(static_cast<int>(wait));
  }
  void schedule(int seconds) {
    if (timer_) ctx_->cancel(timer_);
    timer_ = ctx_->at(ctx_->now() + Duration(static_cast<long long>(seconds) * 1000), [this] { fetch(); });
  }
  ModuleContext* ctx_ = nullptr;
  std::string url_, header_name_, header_value_;
  std::wstring label_, unit_, problem_;
  int decimals_ = 1, interval_s_ = 30;
  std::vector<JsonStep> path_;
  std::optional<Thresholds> thresholds_;
  bool below_ = false;
  TimerId timer_ = 0;
  int failures_ = 0;
  std::optional<std::wstring> text_;
  bool missing_ = false;
  ElementState state_ = ElementState::Normal;
};

}  // namespace

ModuleInfo http_sensor_module_info() {
  ModuleInfo m;
  m.id = "http_sensor";
  m.name = "Web value (JSON)";
  m.description = "One number read from a web address (Home Assistant, NAS…).";
  m.category = "Online data";
  m.hue = "#cf7bff";
  m.slots = {SlotKind::Key};
  m.settings_schema = json::parse(R"J({"fields":[
    {"key":"url","type":"string","default":"","label":"Address","placeholder":"https://…","hint":"http or https."},
    {"key":"json_path","type":"string","default":"","label":"Value path","placeholder":"result.temperature",
     "hint":"Where the number is in the answer, such as items[0].value. Empty: the whole answer."},
    {"key":"label","type":"string","default":"SENSOR","label":"Label"},
    {"key":"unit","type":"string","default":"","label":"Unit"},
    {"key":"decimals","type":"int","default":1,"min":0,"max":3,"label":"Decimals"},
    {"key":"interval_s","type":"int","default":30,"min":5,"max":86400,"label":"Every (s)"},
    {"key":"header_name","type":"string","default":"","label":"Auth header (optional)","placeholder":"Authorization"},
    {"key":"header_value","type":"secret","default":"","label":"Header value","placeholder":"Bearer <token>"},
    {"key":"direction","type":"enum","default":"off","label":"Warn when","group":"thresholds",
     "options":[{"value":"off","label":"Never"},{"value":"above","label":"Above a level"},{"value":"below","label":"Below a level"}]},
    {"key":"warn","type":"number","default":0,"min":-1000000000,"max":1000000000,"label":"Warning at",
     "group":"thresholds","show_if":{"field":"direction","in":["above","below"]}},
    {"key":"crit","type":"number","default":0,"min":-1000000000,"max":1000000000,"label":"Critical at",
     "group":"thresholds","show_if":{"field":"direction","in":["above","below"]}},
    {"key":"hysteresis","type":"number","default":0,"min":0,"max":1000000000,"label":"Clear margin",
     "hint":"Stays in warning until the value is this far back. Avoids flicker.","group":"thresholds",
     "show_if":{"field":"direction","in":["above","below"]}}]})J");
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return std::make_unique<HttpSensorModule>(); };
  return m;
}

}  // namespace ld
