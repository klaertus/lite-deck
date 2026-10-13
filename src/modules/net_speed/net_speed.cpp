// src/modules/net_speed/net_speed.cpp
#include "modules/net_speed/net_speed.h"
#include "modules/common/tap_action.h"
#include "modules/common/state_settings.h"
// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <deque>
#include <format>
#include <unordered_map>
#include "core/log.h"
#include "core/strings.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/key_module.h"

#pragma comment(lib, "iphlpapi.lib")

namespace ld {

std::optional<NetDelta> net_delta(const std::vector<NetCounter>& before, const std::vector<NetCounter>& after) {
  std::unordered_map<std::uint64_t, const NetCounter*> prev;
  for (const NetCounter& c : before) prev[c.id] = &c;
  NetDelta d;
  for (const NetCounter& c : after) {
    const auto it = prev.find(c.id);
    if (it == prev.end()) continue;  // a new interface: counted from the next second
    if (c.rx < it->second->rx || c.tx < it->second->tx) return std::nullopt;
    d.rx += c.rx - it->second->rx;
    d.tx += c.tx - it->second->tx;
  }
  return d;
}

RateText format_rate(double bytes_per_second, bool bits) {
  static constexpr std::array<const wchar_t*, 4> kBytes{L"B/s", L"KB/s", L"MB/s", L"GB/s"};
  static constexpr std::array<const wchar_t*, 4> kBits{L"b/s", L"Kb/s", L"Mb/s", L"Gb/s"};
  double v = std::max(0.0, bytes_per_second) * (bits ? 8.0 : 1.0);
  if (!std::isfinite(v)) v = 0;
  const auto& units = bits ? kBits : kBytes;
  for (std::size_t i = 0; i < units.size(); ++i, v /= 1000.0) {
    const bool last = i + 1 == units.size();
    if (i == 0) {
      const double r = std::round(v);
      if (r < 1000 || last) return {format_decimal(r, 0), units[i]};
      continue;
    }
    const double tenths = std::round(v * 10) / 10;
    const int decimals = tenths < 100 ? 1 : 0;
    const double r = decimals ? tenths : std::round(v);
    if (r < 1000 || last) return {format_decimal(r, decimals), units[i]};
  }
  return {L"0", units[0]};
}

namespace {

std::wstring lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// Virtual adapters that may claim to be hardware: never counted by "auto".
constexpr std::array<const wchar_t*, 9> kVirtual{L"hyper-v", L"virtualbox", L"vmware", L"tap-", L"wintun",
                                                L"wireguard", L"vpn", L"npcap", L"loopback"};

}  // namespace

bool net_interface_selected(const NetInterfaceInfo& itf, const std::string& which) {
  if (which != "auto") {
    const std::wstring w = lower(widen(which));
    return lower(itf.alias) == w || lower(itf.description) == w;
  }
  if (!itf.up || !itf.hardware || itf.loopback || itf.tunnel || itf.filter) return false;
  const std::wstring d = lower(itf.description) + L" " + lower(itf.alias);
  return std::none_of(kVirtual.begin(), kVirtual.end(), [&](const wchar_t* v) { return d.find(v) != std::wstring::npos; });
}

NetReader if_table_reader(std::string which) {
  return [which = std::move(which)]() -> std::optional<std::vector<NetCounter>> {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return std::nullopt;
    std::vector<NetCounter> out;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
      const MIB_IF_ROW2& r = table->Table[i];
      NetInterfaceInfo info{.alias = r.Alias,
                            .description = r.Description,
                            .hardware = r.InterfaceAndOperStatusFlags.HardwareInterface != FALSE,
                            .up = r.OperStatus == IfOperStatusUp,
                            .loopback = r.Type == IF_TYPE_SOFTWARE_LOOPBACK,
                            .tunnel = r.Type == IF_TYPE_TUNNEL,
                            .filter = r.InterfaceAndOperStatusFlags.FilterInterface != FALSE};
      if (net_interface_selected(info, which)) out.push_back(NetCounter{r.InterfaceLuid.Value, r.InOctets, r.OutOctets});
    }
    FreeMibTable(table);
    return out;
  };
}

namespace {

constexpr std::size_t kHistory = 60;
constexpr Rgb kCyan{0x22, 0xd3, 0xee};
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "up", .label = "Upload", .size = 15, .weight = 600, .min_size = 8, .max_size = 30, .row = Row::Top,
     .align = HAlign::Right, .color = kCyan, .role = TextRole::Secondary},
    {.key = "value", .label = "Download", .size = 36, .weight = 700, .min_size = 10, .max_size = 80,
     .row = Row::Middle, .color = kCyan, .role = TextRole::Value},
    {.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
     .kind = ElementKind::Graph, .row = Row::Bottom, .color = kCyan},
    // Over the graph, at its oldest end (drawn after it).
    {.key = "unit", .label = "Unit", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Bottom,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
};

class NetSpeedModule final : public TapKey {
public:
  explicit NetSpeedModule(NetReader reader) : TapKey(kElements), reader_(std::move(reader)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    bits_ = s.at("unit").get<std::string>() == "bits";
    which_ = s.at("interface").get<std::string>();
    if (which_.empty()) which_ = "auto";
    if (!reader_) reader_ = if_table_reader(which_);
    sample();
    ctx.every(Duration(1000), [this] { sample(); });
  }
  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    out.push_back(KeyElement{.key = "label", .text = L"NET"});
    if (!rate_ || failed_) {
      out.push_back(KeyElement{.key = "value", .text = kDash, .state = ElementState::Stale});
      return out;
    }
    const RateText down = format_rate(rate_->rx, bits_);
    const RateText up = format_rate(rate_->tx, bits_);
    out.push_back(KeyElement{.key = "up", .text = L"↑ " + up.number + L" " + up.unit});
    out.push_back(KeyElement{.key = "value", .text = down.number});
    float hi = 1.f;
    for (float v : history_) hi = std::max(hi, v);
    out.push_back(KeyElement{.key = "graph", .points = graph_points(history_, 0.f, hi), .capacity = kHistory});
    out.push_back(KeyElement{.key = "unit", .text = down.unit});
    return out;
  }

private:
  struct Rate {
    double rx = 0, tx = 0;
  };
  void sample() {
    const TimePoint now = ctx_->now();
    std::optional<std::vector<NetCounter>> cur = reader_();
    if (!cur) {
      if (!failed_) log_info("net_speed: the interface table cannot be read; the key shows a dash");
      prev_.reset();
      if (!failed_) ctx_->invalidate();
      failed_ = true;
      return;
    }
    if (!prev_) {  // the first reading, or the first after a failure: the baseline
      prev_ = std::move(cur);
      prev_at_ = now;
      return;
    }
    const auto elapsed = std::chrono::duration<double>(now - prev_at_).count();
    if (elapsed < 0.1) return;  // a catch-up tick: too short to measure, keep the baseline
    const std::optional<NetDelta> d = net_delta(*prev_, *cur);
    prev_ = std::move(cur);
    prev_at_ = now;
    if (!d) return;  // a counter went back: skip this second
    rate_ = Rate{static_cast<double>(d->rx) / elapsed, static_cast<double>(d->tx) / elapsed};
    failed_ = false;
    history_.push_back(static_cast<float>(rate_->rx));
    if (history_.size() > kHistory) history_.pop_front();
    ctx_->invalidate();  // the graph moves
  }
  NetReader reader_;
  ModuleContext* ctx_ = nullptr;
  bool bits_ = false;
  std::string which_ = "auto";
  std::optional<std::vector<NetCounter>> prev_;
  TimePoint prev_at_{};
  std::optional<Rate> rate_;
  bool failed_ = false;
  std::deque<float> history_;
};

}  // namespace

std::unique_ptr<Module> make_net_speed_module(NetReader reader) {
  return std::make_unique<NetSpeedModule>(std::move(reader));
}

ModuleInfo net_speed_module_info() {
  ModuleInfo m;
  m.id = "net_speed";
  m.name = "Network speed";
  m.description = "Download speed with a graph, upload in the corner.";
  m.category = "System";
  m.hue = "#22d3ee";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"J({"fields":[
    {"key":"unit","type":"enum","default":"bytes","label":"Unit",
     "options":[{"value":"bytes","label":"MB/s"},{"value":"bits","label":"Mb/s"}]},
    {"key":"interface","type":"string","default":"auto","label":"Network adapter","placeholder":"auto",
     "hint":"auto: every physical adapter; or a name such as Ethernet or Wi-Fi."}]})J");
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_net_speed_module({}); };
  return m;
}

}  // namespace ld
