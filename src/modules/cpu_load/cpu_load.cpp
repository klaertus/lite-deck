#include "modules/cpu_load/cpu_load.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include "modules/common/tap_action.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/common/temp_header.h"
#include "modules/key_module.h"

namespace ld {

double cpu_busy_percent(const CpuTimes& a, const CpuTimes& b) {
  std::uint64_t total = (b.kernel - a.kernel) + (b.user - a.user);
  std::uint64_t idle = b.idle - a.idle;
  if (total == 0) return 0.0;
  return std::clamp(100.0 * static_cast<double>(total - std::min(idle, total)) / static_cast<double>(total), 0.0, 100.0);
}

static std::uint64_t ft(const FILETIME& f) {
  return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

std::optional<CpuTimes> read_cpu_times() {
  FILETIME idle{}, kernel{}, user{};
  if (!GetSystemTimes(&idle, &kernel, &user)) return std::nullopt;
  return CpuTimes{ft(idle), ft(kernel), ft(user)};
}

namespace {

constexpr std::size_t kHistory = 60;
constexpr ElementSpec kElements[] = {
    {.key = "value", .label = "Percentage", .size = 36, .weight = 700, .min_size = 10, .max_size = 80,
     .row = Row::Middle, .color = kCpuHue, .role = TextRole::Value},
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    tinted(kTempElement, kCpuHue),
    {.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
     .kind = ElementKind::Graph, .row = Row::Bottom, .color = kCpuHue},
};

class CpuLoadModule final : public TapKey {
public:
  explicit CpuLoadModule(CpuTempReader temp_reader) : TapKey(kElements), temp_reader_(std::move(temp_reader)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    show_temp_ = s.value("show_temperature", false);
    thresholds_ = thresholds_from(s, "", kCpuTempThresholds);
    gradient_ = gradient_from(s, kTemperatureGradient);
    if (show_temp_) {
      if (!temp_reader_) temp_reader_ = service_temperature_reader("cpu_load");
      read_temperature();
    }
    prev_ = read_cpu_times().value_or(CpuTimes{});
    ctx.every(Duration(1000), [this] { sample(); });
  }
  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    out.push_back(KeyElement{.key = "value", .text = std::format(L"{:.0f}%", value_)});
    out.push_back(KeyElement{.key = "label", .text = L"CPU"});
    if (show_temp_) {
      KeyElement temp{.key = "temp", .text = format_temperature(temp_), .gradient = gradient_};
      temp.state = temp_ ? temp_state_ : ElementState::Stale;
      if (temp_) temp.metric = static_cast<double>(*temp_);
      out.push_back(std::move(temp));
    }
    out.push_back(KeyElement{.key = "graph", .points = graph_points(history_, 0.f, 100.f), .capacity = kHistory});
    return out;
  }

private:
  void read_temperature() {
    const std::optional<double> t = temp_reader_();
    temp_ = t ? std::optional<int>(static_cast<int>(std::lround(*t))) : std::nullopt;
    // The state from the raw value, like cpu_temp: both keys cross a threshold at the same reading.
    temp_state_ = t ? threshold_state(temp_state_, *t, thresholds_) : ElementState::Normal;  // a gap resets
  }
  void sample() {
    if (const auto now = read_cpu_times()) {  // a failed read keeps the last value
      value_ = cpu_busy_percent(prev_, *now);
      prev_ = *now;
    }
    history_.push_back(static_cast<float>(value_));
    if (history_.size() > kHistory) history_.pop_front();
    if (show_temp_) read_temperature();
    ctx_->invalidate();
  }
  CpuTempReader temp_reader_;
  ModuleContext* ctx_ = nullptr;
  bool show_temp_ = false;
  Thresholds thresholds_ = kCpuTempThresholds;
  Gradient gradient_ = kTemperatureGradient;
  std::optional<int> temp_;
  ElementState temp_state_ = ElementState::Normal;
  CpuTimes prev_;
  double value_ = 0;
  std::deque<float> history_;
};

}  // namespace

std::unique_ptr<Module> make_cpu_load_module(CpuTempReader temp_reader) {
  return std::make_unique<CpuLoadModule>(std::move(temp_reader));
}

// The temperature settings matter only when the key shows the temperature.
const nlohmann::json kWithTemperature{{"field", "show_temperature"}, {"equals", true}};

ModuleInfo cpu_load_module_info() {
  ModuleInfo m;
  m.id = "cpu_load";
  m.name = "CPU load";
  m.description = "CPU use in % with a 1-minute graph; temperature optional.";
  m.category = "System";
  m.hue = "#00e0a0";
  m.slots = {SlotKind::Key};
  m.settings_schema = with_fields(
      with_fields(nlohmann::json::parse(R"J({"fields":[
        {"key":"show_temperature","type":"bool","default":false,"label":"Show the temperature",
         "hint":"Needs the sensors service."}]})J"),
                  shown_if(threshold_fields("", "°C", kCpuTempThresholds, 0, 120, true), kWithTemperature)),
      shown_if(gradient_fields("°C", kTemperatureGradient, -50, 150), kWithTemperature));
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_cpu_load_module({}); };
  return m;
}

}  // namespace ld
