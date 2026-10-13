#include "modules/gpu/gpu.h"
#include <cmath>
#include <deque>
#include <format>
#include <string>
#include "core/log.h"
#include "modules/common/tap_action.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/common/temp_header.h"
#include "modules/key_module.h"

namespace ld {
namespace {

constexpr std::size_t kHistory = 60;
constexpr Thresholds kTempThresholds{75, 85, 3};
constexpr Thresholds kVramThresholds{90, 97, 3};
constexpr Rgb kCoral{0xff, 0x7f, 0x50};  // GPU usage and temperature
constexpr Rgb kPink{0xff, 0x7a, 0xc8};   // VRAM
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10, .max_size = 80, .row = Row::Middle,
     .color_mode = ColorMode::State, .color = kCoral, .gradient = true, .role = TextRole::Value},
    {.key = "unit", .label = "Unit", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Bottom,
     .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
    tinted(kTempElement, kCoral),
    {.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
     .kind = ElementKind::Graph, .row = Row::Bottom, .color = kCoral},
};

enum class Mode { Vram, Temp, Usage };

// The real usage reader: engine counters through a meter owned by this key (two keys on the same
// adapter must not share the previous sample), and the temperature only when the key shows it.
GpuUsageReader real_usage_reader(unsigned index, bool with_temperature) {
  auto meter = std::make_shared<GpuUsageMeter>();
  auto warned = std::make_shared<bool>(false);
  return [=]() -> GpuUsageSample {
    GpuUsageSample out;
    if (auto s = gpu_node_sample(index)) {
      out.percent = meter->update(std::move(*s));
    } else if (!*warned) {
      log_info("gpu: adapter {} publishes no engine statistics, usage shows a dash", index);
      *warned = true;
    }
    if (with_temperature) out.temp_c = gpu_temperature(index);
    return out;
  };
}

class GpuModule final : public TapKey {
public:
  GpuModule(GpuReader reader, GpuUsageReader usage)
      : TapKey(kElements), reader_(std::move(reader)), usage_(std::move(usage)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    const std::string mode = s.at("mode").get<std::string>();
    mode_ = mode == "temp" ? Mode::Temp : mode == "usage" ? Mode::Usage : Mode::Vram;
    show_temp_ = s.value("show_temperature", false);
    temp_thresholds_ = thresholds_from(s, "temp_", kTempThresholds);
    vram_thresholds_ = thresholds_from(s, "vram_", kVramThresholds);
    gradient_ = gradient_from(s, kTemperatureGradient);
    const unsigned index = s.at("index").get<unsigned>();
    if (mode_ == Mode::Usage) {
      if (!usage_) usage_ = real_usage_reader(index, show_temp_);
      sample_usage();
      ctx.every(Duration(1000), [this] { sample_usage(); });
      return;
    }
    if (!reader_) {
      if (mode_ == Mode::Temp) {  // the temperature alone: no VRAM statistics needed
        reader_ = [index]() -> std::optional<GpuReading> {
          const auto t = gpu_temperature(index);
          if (!t) return std::nullopt;
          return GpuReading{.temp_c = t};
        };
      } else {  // the VRAM alone: a driver without a temperature still shows it
        reader_ = [index] { return gpu_read(index); };
      }
    }
    sample();
    ctx.every(Duration(1000), [this] { sample(); });
  }
  std::vector<KeyElement> content() override {
    if (mode_ == Mode::Usage) return usage_content();
    const bool temp_mode = mode_ == Mode::Temp;
    KeyElement value{.key = "value", .text = text_, .state = text_ == kDash ? ElementState::Stale : state_};
    if (temp_mode && metric_) {
      value.metric = metric_;
      value.gradient = gradient_;
    }
    if (!temp_mode) value.color = kPink;
    std::wstring unit = L"°C";
    if (!temp_mode) unit = total_gib_ > 0 ? std::format(L"/ {} GB", std::llround(total_gib_)) : std::wstring(L"GB");
    return {KeyElement{.key = "label", .text = temp_mode ? L"GPU" : L"VRAM"}, std::move(value),
            KeyElement{.key = "unit", .text = std::move(unit)}};
  }

private:
  std::vector<KeyElement> usage_content() const {
    std::vector<KeyElement> out;
    KeyElement value{.key = "value"};
    if (value_) {
      value.text = std::format(L"{:.0f}%", *value_);
    } else {
      value.text = kDash;
      value.state = ElementState::Stale;
    }
    out.push_back(std::move(value));
    out.push_back(KeyElement{.key = "label", .text = L"GPU"});
    if (show_temp_) {
      KeyElement temp{.key = "temp", .text = format_temperature(temp_), .gradient = gradient_};
      temp.state = temp_ ? temp_state_ : ElementState::Stale;
      if (temp_) temp.metric = static_cast<double>(*temp_);
      out.push_back(std::move(temp));
    }
    if (value_)
      out.push_back(KeyElement{.key = "graph", .points = graph_points(history_, 0.f, 100.f), .capacity = kHistory});
    return out;
  }
  // VRAM and temperature modes.
  void sample() {
    const auto r = reader_();
    std::wstring t = kDash;
    ElementState st = ElementState::Normal;
    std::optional<double> metric;
    if (mode_ == Mode::Temp) {
      if (r && r->temp_c) {
        t = std::to_wstring(*r->temp_c);
        metric = static_cast<double>(*r->temp_c);
        st = threshold_state(state_, *metric, temp_thresholds_);
      }
    } else if (r) {
      t = format_decimal(r->vram_used_gib, 1);
      // Below 1 GiB of dedicated memory (an iGPU's reserved carve-out), the resident bytes routinely exceed
      // it: no percentage, no thresholds.
      if (r->vram_total_gib >= 1.0)
        st = threshold_state(state_, 100.0 * r->vram_used_gib / r->vram_total_gib, vram_thresholds_);
    }
    metric_ = metric;
    const double total = r && r->vram_total_gib >= 1.0 ? r->vram_total_gib : 0.0;
    const bool changed = t != text_ || st != state_ || std::llround(total) != std::llround(total_gib_);
    text_ = std::move(t);
    state_ = st;
    total_gib_ = total;
    if (changed) ctx_->invalidate();  // one redraw per visible change, never two
  }
  void sample_usage() {
    const GpuUsageSample s = usage_();
    const bool had = value_.has_value();
    const bool temp_changed = show_temp_ && s.temp_c != temp_;
    value_ = s.percent;
    temp_ = s.temp_c;
    temp_state_ = temp_ ? threshold_state(temp_state_, *temp_, temp_thresholds_) : ElementState::Normal;  // a gap resets
    if (value_) {
      history_.push_back(static_cast<float>(*value_));
      if (history_.size() > kHistory) history_.pop_front();
      ctx_->invalidate();  // the graph moves
    } else {
      history_.clear();
      if (had || temp_changed) ctx_->invalidate();
    }
  }
  GpuReader reader_;
  GpuUsageReader usage_;
  ModuleContext* ctx_ = nullptr;
  Mode mode_ = Mode::Vram;
  bool show_temp_ = false;
  Thresholds temp_thresholds_ = kTempThresholds;
  Thresholds vram_thresholds_ = kVramThresholds;
  Gradient gradient_ = kTemperatureGradient;
  std::wstring text_ = kDash;
  ElementState state_ = ElementState::Normal;
  std::optional<double> metric_;
  double total_gib_ = 0;
  std::optional<double> value_;
  std::optional<int> temp_;
  ElementState temp_state_ = ElementState::Normal;
  std::deque<float> history_;
};

}  // namespace

std::unique_ptr<Module> make_gpu_module(GpuReader reader, GpuUsageReader usage) {
  return std::make_unique<GpuModule>(std::move(reader), std::move(usage));
}

ModuleInfo gpu_module_info() {
  ModuleInfo m;
  m.id = "gpu";
  m.name = "Graphics card";
  m.description = "GPU memory, temperature or usage.";
  m.category = "System";
  m.hue = "#ff7f50";
  m.slots = {SlotKind::Key};
  nlohmann::json schema = nlohmann::json::parse(R"J({"fields":[
    {"key":"mode","type":"enum","default":"vram","label":"Measure","options":[{"value":"vram","label":"Memory used"},
     {"value":"temp","label":"Temperature"},{"value":"usage","label":"Usage"}]},
    {"key":"index","type":"int","default":0,"min":0,"max":7,"label":"Graphics card","hint":"0 = the most powerful one."},
    {"key":"show_temperature","type":"bool","default":false,"label":"Show the temperature",
     "show_if":{"field":"mode","equals":"usage"}}]})J");
  // The temperature settings: in temp mode, and in usage mode when the key shows the temperature.
  const nlohmann::json temperature = nlohmann::json::parse(R"J([{"field":"mode","equals":"temp"},
    {"field":"mode","equals":"usage","and":{"field":"show_temperature","equals":true}}])J");
  schema = with_fields(std::move(schema), shown_if(threshold_fields("temp_", "°C", kTempThresholds, 0, 120, true), temperature));
  schema = with_fields(std::move(schema), shown_if(threshold_fields("vram_", "% used", kVramThresholds, 0, 100, false),
                                                   nlohmann::json{{"field", "mode"}, {"equals", "vram"}}));
  m.settings_schema = with_fields(std::move(schema), shown_if(gradient_fields("°C", kTemperatureGradient, -50, 150), temperature));
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_gpu_module({}); };
  return m;
}

}  // namespace ld
