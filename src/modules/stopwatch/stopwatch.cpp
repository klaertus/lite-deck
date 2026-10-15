// src/modules/stopwatch/stopwatch.cpp
#include "modules/stopwatch/stopwatch.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include "core/log.h"
#include "core/strings.h"
#include "modules/common/key_look.h"
#include "modules/common/wall_clock.h"
#include "modules/key_module.h"
#include "modules/stopwatch/stopwatch_log.h"
#include "modules/time_zone.h"

namespace ld {
namespace {

using sc = std::chrono::system_clock;
long long to_ms(sc::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

bool read_file(const std::filesystem::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

std::tm local_tm(long long ms) {
  std::time_t tt = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  localtime_s(&tm, &tt);
  return tm;
}

constexpr Rgb kRunning{0x4c, 0xd9, 0x64};  // the "running" green of the strip and the seek ring
constexpr ElementSpec kTime{.key = "time", .label = "Time", .size = 28, .weight = 700, .min_size = 10, .max_size = 60,
                            .row = Row::Middle, .color_mode = ColorMode::State, .color = kRunning,
                            .role = TextRole::LongValue};
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    kTime,
    {.key = "state", .label = "Running / paused", .size = 22, .weight = 700, .min_size = 8, .max_size = 40,
     .row = Row::Top, .align = HAlign::Left, .color_mode = ColorMode::State, .color = kRunning, .role = TextRole::Symbol},
    hours_minutes_spec(kTime),
    seconds_spec(kTime),
};

class StopwatchModule final : public KeyModule {
public:
  StopwatchModule() : KeyModule(kElements) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    ctx_ = &ctx;
    file_ = stopwatch_file(s.at("file").get<std::string>());
    below_ = s.at("seconds_below").get<bool>();
    nlohmann::json st = ctx.load_state();
    if (st.is_object()) {
      auto integer = [&st](const char* k, long long& v) {
        if (!st.contains(k) || !st[k].is_number_integer()) return false;
        v = st[k].get<long long>();
        return true;
      };
      if (st.contains("running") && st["running"].is_boolean()) running_ = st["running"].get<bool>();
      if (!integer("started_ms", started_ms_)) running_ = false;
      integer("accumulated_ms", accumulated_ms_);
      integer("session_start_ms", session_start_ms_);
      integer("paused_ms", paused_ms_);
      integer("paused_at_ms", paused_at_ms_);
      integer("pauses", pauses_);
      if (accumulated_ms_ < 0) accumulated_ms_ = 0;
      if (paused_ms_ < 0) paused_ms_ = 0;
      if (pauses_ < 0) pauses_ = 0;
      if (session_start_ms_ < 0) session_start_ms_ = 0;
    }
    // A session saved before the CSV log (no start recorded) started when its time began, and a paused
    // one counts as paused from now on. A paused session missing its pause time is repaired the same way.
    const long long now = to_ms(wall_now());
    if (session_start_ms_ == 0 && (running_ || accumulated_ms_ > 0)) {
      session_start_ms_ = (running_ ? started_ms_ : now) - accumulated_ms_;
      if (session_start_ms_ <= 0) session_start_ms_ = 1;
      paused_at_ms_ = 0;
    }
    if (session_start_ms_ && !running_ && paused_at_ms_ <= 0) paused_at_ms_ = now;
    if (running_) start_ticking();
  }

  void on_event(const InputEvent& e) override {
    if (e.kind == EventKind::Tap) {
      if (overlay_ != Overlay::None) {
        ctx_->cancel(overlay_timer_);
        overlay_timer_ = 0;
        overlay_ = Overlay::None;
      }
      toggle();
    }
    else if (e.kind == EventKind::LongPress) long_press();
  }

  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    if (overlay_ == Overlay::Total) {
      out.push_back(KeyElement{.key = "label", .text = L"Total"});
      add_time(out, total_s_, ElementState::Normal);
      return out;
    }
    const long long ms = elapsed_ms();
    const ElementState st = running_ ? ElementState::Normal : ms > 0 ? ElementState::Warning : ElementState::Idle;
    if (overlay_ != Overlay::None)
      out.push_back(KeyElement{.key = "label", .text = overlay_ == Overlay::Saved ? L"✓ saved" : L"Error"});
    if (running_ || ms > 0)
      out.push_back(KeyElement{.key = "state", .text = running_ ? L"►" : L"❚❚", .state = st});
    add_time(out, ms / 1000, st);
    return out;
  }

private:
  enum class Overlay { None, Saved, Error, Total };

  // "01:02:03" as one element, or "01:02" with "03" below.
  void add_time(std::vector<KeyElement>& out, long long s, ElementState st) const {
    std::wstring hms = widen(format_hms(s));
    if (!below_) {
      out.push_back(KeyElement{.key = "time", .text = std::move(hms), .state = st});
      return;
    }
    out.push_back(KeyElement{.key = "hours_minutes", .text = hms.substr(0, hms.size() - 3), .state = st});
    out.push_back(KeyElement{.key = "seconds", .text = hms.substr(hms.size() - 2), .state = st});
  }

  long long elapsed_ms() const {
    long long run = running_ ? to_ms(wall_now()) - started_ms_ : 0;
    return accumulated_ms_ + (run > 0 ? run : 0);
  }

  // Paused time of the session so far. Wall-clock based like elapsed_ms(): time while the daemon was
  // down counts as paused if the stopwatch was paused, and as counted time if it was running.
  long long paused_total_ms() const {
    long long current = !running_ && session_start_ms_ ? to_ms(wall_now()) - paused_at_ms_ : 0;
    return paused_ms_ + (current > 0 ? current : 0);
  }

  void persist() {
    ctx_->save_state({{"running", running_},
                      {"started_ms", started_ms_},
                      {"accumulated_ms", accumulated_ms_},
                      {"session_start_ms", session_start_ms_},
                      {"paused_ms", paused_ms_},
                      {"paused_at_ms", paused_at_ms_},
                      {"pauses", pauses_}});
  }

  void reset() {
    accumulated_ms_ = started_ms_ = session_start_ms_ = paused_ms_ = paused_at_ms_ = pauses_ = 0;
    running_ = false;
    ctx_->cancel(tick_);
    tick_ = 0;
  }

  void toggle() {
    const long long now = to_ms(wall_now());
    if (running_) {
      accumulated_ms_ = elapsed_ms();
      running_ = false;
      paused_at_ms_ = now;
      ++pauses_;
      ctx_->cancel(tick_);
      tick_ = 0;
    } else {
      if (session_start_ms_ == 0) {  // a new session
        session_start_ms_ = now;
        paused_ms_ = 0;
        pauses_ = 0;
      } else {
        paused_ms_ = paused_total_ms();
      }
      paused_at_ms_ = 0;
      started_ms_ = now;
      running_ = true;
      start_ticking();
    }
    persist();
    ctx_->invalidate();
  }

  void long_press() {
    long long ms = elapsed_ms();
    if (!running_ && ms == 0) {
      if (session_start_ms_) {  // started and paused within the same instant: nothing to keep
        reset();
        persist();
      }
      std::string content;
      std::error_code ec;
      const bool missing = !std::filesystem::exists(file_, ec) && !ec;
      if (!read_file(file_, content) && !missing) {  // locked or denied: not an empty log
        log_error("stopwatch: cannot read {}", narrow(file_.wstring()));
        show(Overlay::Error, Duration(3000));
        return;
      }
      total_s_ = sum_csv_active(content);
      show(Overlay::Total, Duration(5000));
      return;
    }
    refresh_time_zone();
    CsvRow row;
    row.start = local_tm(session_start_ms_);
    row.end = local_tm(to_ms(wall_now()));
    row.active_s = ms / 1000;
    row.paused_s = paused_total_ms() / 1000;
    row.pauses = pauses_;
    // Missing folder, file open in Excel: the session is kept so that the user can retry.
    if (const unsigned long err = append_csv_row(file_, row)) {
      log_error("stopwatch: cannot write {} (error {})", narrow(file_.wstring()), err);
      show(Overlay::Error, Duration(3000));
      return;
    }
    reset();
    persist();
    show(Overlay::Saved, Duration(1500));
  }

  void show(Overlay o, Duration d) {
    overlay_ = o;
    ctx_->invalidate();
    ctx_->cancel(overlay_timer_);
    overlay_timer_ = ctx_->at(ctx_->now() + d, [this] {
      overlay_timer_ = 0;
      overlay_ = Overlay::None;
      ctx_->invalidate();
    });
  }

  void start_ticking() {
    if (!tick_) tick_ = ctx_->every(Duration(1000), [this] { ctx_->invalidate(); });
  }

  ModuleContext* ctx_ = nullptr;
  std::filesystem::path file_;
  bool below_ = false;
  bool running_ = false;
  long long started_ms_ = 0;
  long long accumulated_ms_ = 0;
  long long session_start_ms_ = 0;  // wall ms of the session's first start, 0: no session
  long long paused_ms_ = 0;         // paused time of the finished pauses
  long long paused_at_ms_ = 0;      // wall ms the current pause began
  long long pauses_ = 0;
  long long total_s_ = 0;
  Overlay overlay_ = Overlay::None;
  TimerId tick_ = 0, overlay_timer_ = 0;
};

}  // namespace

std::filesystem::path stopwatch_file(const std::string& setting) {
  PWSTR raw = nullptr;
  std::filesystem::path docs;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw))) docs = raw;
  CoTaskMemFree(raw);
  if (docs.empty()) {  // never the working directory (System32 under Task Scheduler): the profile folder
    wchar_t profile[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n > 0 && n < MAX_PATH) docs = std::wstring(profile, n);
  }
  if (setting.empty()) return docs / L"stopwatch_log.csv";
  std::wstring w = widen(setting);
  const DWORD needed = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);  // "%USERPROFILE%\x.csv"
  if (needed > 0) {
    std::wstring expanded(needed, L'\0');
    const DWORD got = ExpandEnvironmentStringsW(w.c_str(), expanded.data(), needed);
    if (got > 0 && got <= needed) {
      expanded.resize(got - 1);  // drop the terminating NUL
      w = std::move(expanded);
    }
  }
  std::filesystem::path p(w);
  return p.is_absolute() ? p : docs / p;  // relative paths live in Documents, like the default
}

ModuleInfo stopwatch_module_info() {
  ModuleInfo m;
  m.id = "stopwatch";
  m.name = "Stopwatch";
  m.description = "Tap: start or stop. Hold: save to a CSV log (at 0: the total).";
  m.category = "Time";
  m.hue = "#4cd964";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"json({"fields":[
    {"key":"file","type":"string","default":"","label":"CSV log file","placeholder":"Documents\\stopwatch_log.csv",
     "hint":"The CSV file (; separated, for Excel) a long press adds a line to. Empty: the file shown. Its folder must exist."},
    {"key":"seconds_below","type":"bool","default":false,"label":"Seconds on a line below",
     "hint":"Hours and minutes large, the seconds smaller underneath."}]})json");
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return std::make_unique<StopwatchModule>(); };
  return m;
}

}  // namespace ld
