// src/modules/disk/disk.cpp
#include "modules/common/tap_action.h"
#include "modules/disk/disk.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <format>
#include "core/log.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/key_module.h"

#pragma comment(lib, "pdh.lib")

namespace ld {

std::optional<std::wstring> normalize_drive(const std::string& setting) {
  const auto b = setting.find_first_not_of(" \t");
  if (b == std::string::npos) return std::nullopt;
  std::string s = setting.substr(b, setting.find_last_not_of(" \t") - b + 1);
  if (s.size() >= 2 && s[1] == ':') s.erase(1, 1);
  if (s.size() == 2 && (s[1] == '\\' || s[1] == '/')) s.erase(1, 1);
  if (s.size() != 1) return std::nullopt;
  const char c = s[0];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return std::nullopt;
  return std::wstring(1, static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)))) + L":";
}

std::wstring format_disk_rate(double bytes_per_second) {
  double mb = std::max(0.0, bytes_per_second) / 1e6;
  if (!std::isfinite(mb)) mb = 0;
  const double tenths = std::round(mb * 10) / 10;
  return tenths < 100 ? format_decimal(tenths, 1) : format_decimal(std::round(mb), 0);
}

std::optional<DiskSpace> read_disk_space(const std::wstring& root) {
  ULARGE_INTEGER avail{}, total{}, free{};
  if (!GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free) || total.QuadPart == 0) return std::nullopt;
  return DiskSpace{total.QuadPart, free.QuadPart};
}

DiskIoReader pdh_disk_io_reader(std::wstring drive) {
  struct Query {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER read = nullptr, write = nullptr;
    int failures = 0;
    ULONGLONG retry_at = 0;
    void close() {
      if (q) PdhCloseQuery(q);
      q = nullptr;
    }
    ~Query() { close(); }
  };
  auto st = std::make_shared<Query>();
  return [st, drive = std::move(drive)]() -> std::optional<DiskIo> {
    Query& s = *st;
    if (!s.q) {
      if (GetTickCount64() < s.retry_at) return std::nullopt;
      const std::wstring base = L"\\LogicalDisk(" + drive + L")\\";
      if (PdhOpenQueryW(nullptr, 0, &s.q) != ERROR_SUCCESS ||
          PdhAddEnglishCounterW(s.q, (base + L"Disk Read Bytes/sec").c_str(), 0, &s.read) != ERROR_SUCCESS ||
          PdhAddEnglishCounterW(s.q, (base + L"Disk Write Bytes/sec").c_str(), 0, &s.write) != ERROR_SUCCESS ||
          PdhCollectQueryData(s.q) != ERROR_SUCCESS) {
        s.close();
        s.retry_at = GetTickCount64() + 30'000;  // an absent drive: try again in 30 s
        return std::nullopt;
      }
      s.failures = 0;
      return std::nullopt;  // the first sample: no rate yet
    }
    PDH_FMT_COUNTERVALUE r{}, w{};
    if (PdhCollectQueryData(s.q) != ERROR_SUCCESS ||
        PdhGetFormattedCounterValue(s.read, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &r) != ERROR_SUCCESS ||
        PdhGetFormattedCounterValue(s.write, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &w) != ERROR_SUCCESS ||
        r.CStatus != PDH_CSTATUS_VALID_DATA || w.CStatus != PDH_CSTATUS_VALID_DATA) {
      if (++s.failures >= 30) {  // the drive went away: build the query again later
        s.close();
        s.retry_at = GetTickCount64() + 30'000;
      }
      return std::nullopt;
    }
    s.failures = 0;
    return DiskIo{r.doubleValue, w.doubleValue};
  };
}

namespace {

constexpr std::size_t kHistory = 60;
constexpr Duration kSpacePeriod{30'000};
constexpr Rgb kIndigo{0x7c, 0x83, 0xff};
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Drive", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "write", .label = "Write (I/O)", .size = 15, .weight = 600, .min_size = 8, .max_size = 30, .row = Row::Top,
     .align = HAlign::Right, .color = kIndigo, .role = TextRole::Secondary},
    {.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10, .max_size = 80, .row = Row::Middle,
     .color_mode = ColorMode::State, .color = kIndigo, .role = TextRole::Value},
    {.key = "graph", .label = "Graph (I/O)", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
     .kind = ElementKind::Graph, .row = Row::Bottom, .color = kIndigo},
    // Space: used / total, centred as on the RAM key.
    {.key = "unit", .label = "Used / total (space)", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
     .row = Row::Bottom, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
    // I/O: over the graph, at its oldest end (drawn after it).
    {.key = "rate_unit", .label = "Unit (I/O)", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
     .row = Row::Bottom, .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true,
     .role = TextRole::Secondary},
};

double gib(std::uint64_t bytes) { return static_cast<double>(bytes) / static_cast<double>(1ull << 30); }

class DiskModule final : public TapKey {
public:
  DiskModule(DiskSpaceReader space, DiskIoReader io, Runner runner)
      : TapKey(kElements),
        space_reader_(std::move(space)), io_reader_(std::move(io)), runner_(std::move(runner)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    io_ = s.at("mode").get<std::string>() == "io";
    drive_ = normalize_drive(s.at("drive").get<std::string>());
    const double warn = s.at("free_warn").get<double>();
    const double crit = std::min(s.at("free_crit").get<double>(), warn);
    // On the free space, the lower the worse: thresholds on its opposite.
    thresholds_ = Thresholds{-warn, -crit, std::max(0.0, s.at("hysteresis").get<double>())};
    ctx.invalidate();
    if (!drive_) return;
    if (!runner_) runner_ = worker_runner();
    if (io_ && !io_reader_) io_reader_ = pdh_disk_io_reader(*drive_);
    if (!io_ && !space_reader_) space_reader_ = read_disk_space;
    read();
    ctx.every(io_ ? Duration(1000) : kSpacePeriod, [this] { read(); });
  }
  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    out.push_back(KeyElement{.key = "label", .text = drive_ ? *drive_ : std::wstring(L"?:")});
    if (!drive_) {
      out.push_back(KeyElement{.key = "value", .text = L"Drive?", .state = ElementState::Idle});
      return out;
    }
    if (io_ ? !rates_ : !space_) {
      out.push_back(KeyElement{.key = "value", .text = kDash, .state = ElementState::Stale});
      return out;
    }
    if (io_) {
      out.push_back(KeyElement{.key = "write", .text = L"W " + format_disk_rate(rates_->write) + L" MB/s"});
      out.push_back(KeyElement{.key = "value", .text = format_disk_rate(rates_->read)});
      float hi = 1.f;
      for (float v : history_) hi = std::max(hi, v);
      out.push_back(KeyElement{.key = "graph", .points = graph_points(history_, 0.f, hi), .capacity = kHistory});
      out.push_back(KeyElement{.key = "rate_unit", .text = L"R MB/s"});
      return out;
    }
    const std::uint64_t used = space_->total - std::min(space_->free, space_->total);
    const double percent = 100.0 * static_cast<double>(used) / static_cast<double>(space_->total);
    out.push_back(KeyElement{.key = "value", .text = std::format(L"{}%", std::llround(percent)), .state = state_});
    out.push_back(KeyElement{.key = "unit",
                             .text = std::format(L"{} / {} GB", std::llround(gib(used)), std::llround(gib(space_->total)))});
    return out;
  }

private:
  void read() {
    if (busy_) return;  // one reading in flight at a time
    busy_ = true;
    Poster p = ctx_->poster();
    if (io_) {
      runner_([io = io_reader_, p, this] {
        const std::optional<DiskIo> r = io();
        p.post([this, r] { on_io(r); });
      });
    } else {
      runner_([space = space_reader_, root = *drive_ + L"\\", p, this] {
        const std::optional<DiskSpace> r = space(root);
        p.post([this, r] { on_space(r); });
      });
    }
  }
  void on_space(const std::optional<DiskSpace>& r) {
    busy_ = false;
    if (r && r->total > 0) {
      const double free_percent = 100.0 * static_cast<double>(std::min(r->free, r->total)) / static_cast<double>(r->total);
      state_ = threshold_state(state_, -free_percent, thresholds_);
    } else {
      state_ = ElementState::Normal;
    }
    const bool changed = !same(space_, r);
    space_ = r && r->total > 0 ? r : std::nullopt;
    if (changed) ctx_->invalidate();
  }
  void on_io(const std::optional<DiskIo>& r) {
    busy_ = false;
    if (!r) {
      if (rates_) ctx_->invalidate();
      rates_.reset();
      return;
    }
    rates_ = r;
    history_.push_back(static_cast<float>(r->read + r->write));
    if (history_.size() > kHistory) history_.pop_front();
    ctx_->invalidate();  // the graph moves
  }
  static bool same(const std::optional<DiskSpace>& a, const std::optional<DiskSpace>& b) {
    if (!a || !b) return !a && !b;
    return a->total == b->total && a->free == b->free;
  }
  DiskSpaceReader space_reader_;
  DiskIoReader io_reader_;
  Runner runner_;
  ModuleContext* ctx_ = nullptr;
  bool io_ = false;
  std::optional<std::wstring> drive_;
  Thresholds thresholds_;
  bool busy_ = false;
  std::optional<DiskSpace> space_;
  ElementState state_ = ElementState::Normal;
  std::optional<DiskIo> rates_;
  std::deque<float> history_;
};

}  // namespace

std::unique_ptr<Module> make_disk_module(DiskSpaceReader space, DiskIoReader io, Runner runner) {
  return std::make_unique<DiskModule>(std::move(space), std::move(io), std::move(runner));
}

ModuleInfo disk_module_info() {
  ModuleInfo m;
  m.id = "disk";
  m.name = "Disk";
  m.description = "Free space of a drive, or its read and write speed.";
  m.category = "System";
  m.hue = "#7c83ff";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"J({"fields":[
    {"key":"mode","type":"enum","default":"space","label":"Measure",
     "options":[{"value":"space","label":"Free space"},{"value":"io","label":"Read & write speed"}]},
    {"key":"drive","type":"string","default":"C:","label":"Drive","placeholder":"C:","hint":"A drive letter."},
    {"key":"free_warn","type":"number","default":20,"min":0,"max":100,"label":"Warning below (% free)",
     "group":"thresholds","show_if":{"field":"mode","equals":"space"}},
    {"key":"free_crit","type":"number","default":10,"min":0,"max":100,"label":"Critical below (% free)",
     "group":"thresholds","show_if":{"field":"mode","equals":"space"}},
    {"key":"hysteresis","type":"number","default":2,"min":0,"max":20,"label":"Clear margin",
     "hint":"Stays in warning until the value is this far back. Avoids flicker.","group":"thresholds",
     "show_if":{"field":"mode","equals":"space"}}]})J");
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_disk_module({}, {}, {}); };
  return m;
}

}  // namespace ld
