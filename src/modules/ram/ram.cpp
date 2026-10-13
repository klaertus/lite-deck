#include "modules/ram/ram.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include "modules/common/tap_action.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/key_module.h"

namespace ld {

double used_gib(std::uint64_t total, std::uint64_t avail) {
  if (avail >= total) return 0.0;
  return static_cast<double>(total - avail) / static_cast<double>(1ull << 30);
}

namespace {

constexpr Thresholds kThresholds{85, 95, 3};
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10, .max_size = 80, .row = Row::Middle,
     .color_mode = ColorMode::State, .color = Rgb{0x5a, 0xa9, 0xff}, .role = TextRole::Value},
    {.key = "unit", .label = "Unit", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Bottom,
     .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
};

class RamModule final : public TapKey {
public:
  explicit RamModule(MemoryReader reader) : TapKey(kElements), reader_(std::move(reader)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    thresholds_ = thresholds_from(s, "", kThresholds);
    sample();
    ctx.every(Duration(1000), [this] { sample(); });
  }
  std::vector<KeyElement> content() override {
    const bool dash = text_ == kDash;
    const std::wstring unit = total_gib_ > 0 ? std::format(L"/ {} GB", std::llround(total_gib_)) : std::wstring(L"GB");
    return {KeyElement{.key = "label", .text = L"RAM"},
            KeyElement{.key = "value", .text = text_, .state = dash ? ElementState::Stale : state_},
            KeyElement{.key = "unit", .text = unit}};
  }

private:
  void sample() {
    std::optional<std::pair<std::uint64_t, std::uint64_t>> mem;
    if (reader_) {
      mem = reader_();
    } else {
      MEMORYSTATUSEX ms{};
      ms.dwLength = sizeof(ms);
      if (GlobalMemoryStatusEx(&ms)) mem = std::make_pair(ms.ullTotalPhys, ms.ullAvailPhys);
    }
    std::wstring t = mem ? format_decimal(used_gib(mem->first, mem->second), 1) : std::wstring(kDash);
    ElementState st = ElementState::Normal;
    if (mem && mem->first > 0) {
      const double used = static_cast<double>(mem->first - std::min(mem->second, mem->first));
      const double percent = 100.0 * used / static_cast<double>(mem->first);
      st = threshold_state(state_, percent, thresholds_);
    }
    const double total = mem ? static_cast<double>(mem->first) / static_cast<double>(1ull << 30) : 0.0;
    const bool changed = t != text_ || st != state_ || std::llround(total) != std::llround(total_gib_);
    text_ = std::move(t);
    state_ = st;
    total_gib_ = total;
    if (changed) ctx_->invalidate();  // one redraw per visible change, never two
  }
  MemoryReader reader_;
  ModuleContext* ctx_ = nullptr;
  Thresholds thresholds_ = kThresholds;
  std::wstring text_ = kDash;
  ElementState state_ = ElementState::Normal;
  double total_gib_ = 0;
};

}  // namespace

std::unique_ptr<Module> make_ram_module(MemoryReader reader) {
  return std::make_unique<RamModule>(std::move(reader));
}

ModuleInfo ram_module_info() {
  ModuleInfo m;
  m.id = "ram";
  m.name = "Memory (RAM)";
  m.description = "Memory used, in GB of the total.";
  m.category = "System";
  m.hue = "#5aa9ff";
  m.slots = {SlotKind::Key};
  m.settings_schema = with_fields(nlohmann::json::parse(R"({"fields":[]})"),
                                  threshold_fields("", "% used", kThresholds, 0, 100, true));
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_ram_module({}); };
  return m;
}

}  // namespace ld
