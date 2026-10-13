#include "modules/cpu_temp/cpu_temp.h"
#include <cmath>
#include <deque>
#include <format>
#include <string>
#include "core/log.h"
#include "modules/common/tap_action.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/key_module.h"
#include "sensors/sensor_block.h"
#include "sensors/shared_mapping.h"

namespace ld {

CpuTempReader service_temperature_reader(std::string_view log_tag) {
  struct State {
    std::unique_ptr<sensors::BlockReader> reader;
    std::string tag;
    std::string last = "init";
  };
  auto st = std::make_shared<State>();
  st->tag = std::string(log_tag);
  return [st]() -> std::optional<double> {
    if (!st->reader) st->reader = sensors::BlockReader::open(sensors::kMappingName);
    std::optional<double> t;
    std::string state;
    if (!st->reader) {
      state = "service-absent";
    } else {
      auto snap = sensors::read_block(st->reader->block());
      t = sensors::fresh_temperature(snap, sensors::unix_now_ms());
      if (t) state = "ok";
      else if (!snap) state = "busy";
      else if (snap->status != sensors::Status::Ok) state = std::string(sensors::status_name(snap->status));
      else state = "stale";
    }
    if (state != st->last) {
      log_info("{}: {}", st->tag, state);
      st->last = state;
    }
    return t;
  };
}

namespace {

constexpr std::size_t kHistory = 60;
constexpr float kGraphLo = 30.f, kGraphHi = 95.f;
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "value", .label = "Temperature", .size = 36, .weight = 700, .min_size = 10, .max_size = 80,
     .row = Row::Middle, .color_mode = ColorMode::State, .color = kCpuHue, .gradient = true, .role = TextRole::Value},
    {.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
     .kind = ElementKind::Graph, .row = Row::Bottom, .color = kCpuHue},
};

class CpuTempModule final : public TapKey {
public:
  explicit CpuTempModule(CpuTempReader reader) : TapKey(kElements), reader_(std::move(reader)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    thresholds_ = thresholds_from(s, "", kCpuTempThresholds);
    gradient_ = gradient_from(s, kTemperatureGradient);
    if (!reader_) reader_ = service_temperature_reader("cpu_temp");
    sample();
    ctx.every(Duration(1000), [this] { sample(); });
  }
  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    out.push_back(KeyElement{.key = "label", .text = L"CPU TEMP"});
    KeyElement value{.key = "value", .gradient = gradient_};
    if (value_) {
      value.text = std::format(L"{}°C", std::lround(*value_));
      value.state = state_;
      value.metric = *value_;
    } else {
      value.text = kDash;
      value.state = ElementState::Stale;
    }
    out.push_back(std::move(value));
    if (value_)
      out.push_back(KeyElement{.key = "graph", .points = graph_points(history_, kGraphLo, kGraphHi), .capacity = kHistory});
    return out;
  }

private:
  void sample() {
    const std::optional<double> t = reader_();
    if (t) {
      value_ = t;
      state_ = threshold_state(state_, *t, thresholds_);
      history_.push_back(static_cast<float>(*t));
      if (history_.size() > kHistory) history_.pop_front();
      ctx_->invalidate();
    } else if (value_) {
      value_.reset();
      state_ = ElementState::Normal;
      history_.clear();
      ctx_->invalidate();
    }
  }
  CpuTempReader reader_;
  ModuleContext* ctx_ = nullptr;
  Thresholds thresholds_ = kCpuTempThresholds;
  Gradient gradient_ = kTemperatureGradient;
  std::optional<double> value_;
  ElementState state_ = ElementState::Normal;
  std::deque<float> history_;
};

}  // namespace

std::unique_ptr<Module> make_cpu_temp_module(CpuTempReader reader) {
  return std::make_unique<CpuTempModule>(std::move(reader));
}

ModuleInfo cpu_temp_module_info() {
  ModuleInfo m;
  m.id = "cpu_temp";
  m.name = "CPU temperature";
  m.description = "CPU temperature with a graph (needs the sensors service).";
  m.category = "System";
  m.hue = "#00e0a0";
  m.slots = {SlotKind::Key};
  m.settings_schema = with_fields(with_fields(nlohmann::json::parse(R"({"fields":[]})"),
                                              threshold_fields("", "°C", kCpuTempThresholds, 0, 120, true)),
                                  gradient_fields("°C", kTemperatureGradient, -50, 150));
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_cpu_temp_module({}); };
  return m;
}

}  // namespace ld
