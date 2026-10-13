// tests/module_fixtures.h: every rendering module started in a fixed, reproducible state (fake loop,
// fake context, fake sources, injected readers). Shared by the reference images and the appearance
// tests.
#pragma once
#include <doctest/doctest.h>
#include <chrono>
#include <ctime>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include "fake_context.h"
#include "modules/all_modules.h"
#include "modules/common/wall_clock.h"
#include "modules/key_module.h"
#include "modules/schema.h"

namespace fixtures {

struct Rig {
  FakeLoop loop;
  FakeContext ctx{loop};
  std::unique_ptr<ld::Module> m;
  // Starts `module` with the schema defaults of `info` plus `over`, then runs what it posted.
  void start(const ld::ModuleInfo& info, std::unique_ptr<ld::Module> module,
             const nlohmann::json& over = nlohmann::json::object()) {
    nlohmann::json s;
    REQUIRE_FALSE(ld::apply_schema(info.settings_schema, over, s));
    m = std::move(module);
    m->start(ctx, s);
    loop.drain();
  }
};

// Freezes the clock and date modules at a local time; gives the real clock back when destroyed.
struct FrozenWallClock {
  FrozenWallClock(int year, int month, int day, int hour, int minute, int second) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    t.tm_isdst = -1;
    const auto at = std::chrono::system_clock::from_time_t(std::mktime(&t));
    ld::set_wall_clock([at] { return at; });
  }
  ~FrozenWallClock() { ld::set_wall_clock({}); }
  FrozenWallClock(const FrozenWallClock&) = delete;
  FrozenWallClock& operator=(const FrozenWallClock&) = delete;
};

inline std::unique_ptr<Rig> clock_rig(bool seconds, bool seconds_below = false) {
  auto r = std::make_unique<Rig>();
  auto info = ld::clock_module_info();
  r->start(info, info.create(), {{"seconds", seconds}, {"seconds_below", seconds_below}});
  return r;
}

inline std::unique_ptr<Rig> date_rig() {
  auto r = std::make_unique<Rig>();
  auto info = ld::date_module_info();
  r->start(info, info.create());
  return r;
}

inline std::unique_ptr<Rig> cpu_load_rig() {  // just started: "0%", no graph yet
  auto r = std::make_unique<Rig>();
  auto info = ld::cpu_load_module_info();
  r->start(info, info.create());
  return r;
}

inline std::unique_ptr<Rig> ram_rig(std::optional<double> used_gib) {
  auto r = std::make_unique<Rig>();
  r->start(ld::ram_module_info(),
           ld::make_ram_module([used_gib]() -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
             if (!used_gib) return std::nullopt;
             const std::uint64_t total = 32ull << 30;
             return std::make_pair(total, total - static_cast<std::uint64_t>(*used_gib * static_cast<double>(1ull << 30)));
           }));
  return r;
}

inline std::unique_ptr<Rig> gpu_rig(const char* mode, std::optional<ld::GpuReading> reading) {
  auto r = std::make_unique<Rig>();
  r->start(ld::gpu_module_info(), ld::make_gpu_module([reading] { return reading; }), {{"mode", mode}});
  return r;
}

enum class Quote { Waiting, Fresh, Stale };

inline std::unique_ptr<Rig> crypto_rig(Quote q) {
  auto r = std::make_unique<Rig>();
  auto info = ld::crypto_gold_module_info();
  r->start(info, info.create());
  if (q != Quote::Waiting)
    r->ctx.respond(0, ld::HttpResponse{200, R"({"bitcoin":{"eur":98512.4},"tether-gold":{"eur":2431.7}})", ""});
  if (q == Quote::Stale) {
    r->m->on_event(ld::InputEvent{ld::EventKind::Tap});  // refresh, which fails: last prices greyed
    r->ctx.respond(1, ld::HttpResponse{503, "", ""});
  }
  return r;
}

enum class Sky { Waiting, Sunny, UnknownCity };

inline std::unique_ptr<Rig> weather_rig(Sky sky) {
  auto r = std::make_unique<Rig>();
  auto info = ld::weather_module_info();
  r->start(info, info.create(), {{"city", "Brussels"}});  // the default city is empty
  if (sky == Sky::UnknownCity) r->ctx.respond(0, ld::HttpResponse{200, R"({"results":[]})", ""});
  if (sky == Sky::Sunny) {
    r->ctx.respond(0, ld::HttpResponse{200, R"({"results":[{"latitude":50.8503,"longitude":4.3517}]})", ""});
    r->ctx.respond(1, ld::HttpResponse{200, R"({"current":{"temperature_2m":18.4,"weather_code":0},
      "daily":{"temperature_2m_max":[21.2],"temperature_2m_min":[8.6]}})", ""});
  }
  return r;
}

// The weather key with the icon and the temperature in white (what the colour setting #ffffff did
// in a v1 config).
inline ld::SlotLook white_weather() {
  ld::SlotLook look;
  look.elements["icon"].color = ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{255, 255, 255}};
  look.elements["temp"].color = ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{255, 255, 255}};
  return look;
}

inline std::unique_ptr<Rig> stopwatch_rig(long long accumulated_ms, bool seconds_below = false) {  // stopped
  auto r = std::make_unique<Rig>();
  if (accumulated_ms) r->ctx.state = {{"running", false}, {"started_ms", 0}, {"accumulated_ms", accumulated_ms}};
  auto info = ld::stopwatch_module_info();
  r->start(info, info.create(), {{"seconds_below", seconds_below}});
  return r;
}

struct NowPlayingFake final : ld::NowPlayingSource {
  std::function<void(std::optional<ld::Track>)> cb;
  void start(std::function<void(std::optional<ld::Track>)> on_change) override { cb = std::move(on_change); }
};

inline std::unique_ptr<Rig> now_playing_rig(std::optional<ld::Track> track) {
  auto r = std::make_unique<Rig>();
  auto source = std::make_unique<NowPlayingFake>();
  NowPlayingFake* src = source.get();
  auto holder = std::make_shared<std::unique_ptr<ld::NowPlayingSource>>(std::move(source));
  r->start(ld::now_playing_module_info(), ld::make_now_playing_module([holder] { return std::move(*holder); }));
  if (track) {
    src->cb(*track);
    r->loop.drain();
  }
  return r;
}

struct NotificationsFake final : ld::NotificationSource {
  ld::NotifAccess access = ld::NotifAccess::Allowed;
  std::vector<ld::IncomingNotification> list;
  void request_access(std::function<void(ld::NotifAccess)> done) override { done(access); }
  void check_access(std::function<void(ld::NotifAccess)> done) override { done(access); }
  void fetch(std::function<void(std::optional<std::vector<ld::IncomingNotification>>)> done) override { done(list); }
  void remove(std::uint32_t) override {}
};

inline std::unique_ptr<Rig> notifications_rig(ld::NotifAccess access, std::vector<ld::IncomingNotification> list = {}) {
  auto r = std::make_unique<Rig>();
  r->start(ld::notifications_module_info(), ld::make_notifications_module([access, list] {
             auto s = std::make_unique<NotificationsFake>();
             s->access = access;
             s->list = list;
             return s;
           }));
  return r;
}

inline std::unique_ptr<Rig> cpu_load_temp_rig(double celsius) {  // just started, temperature shown
  auto r = std::make_unique<Rig>();
  r->start(ld::cpu_load_module_info(),
           ld::make_cpu_load_module([celsius] { return std::optional<double>(celsius); }),
           {{"show_temperature", true}});
  return r;
}

inline std::unique_ptr<Rig> gpu_usage_rig() {  // 37 % for 4 s at 61 °C: a flat graph
  auto r = std::make_unique<Rig>();
  r->start(ld::gpu_module_info(),
           ld::make_gpu_module([] { return std::optional<ld::GpuReading>{}; },
                               [] { return ld::GpuUsageSample{37.0, 61}; }),
           {{"mode", "usage"}, {"show_temperature", true}});
  r->loop.advance(ld::Duration(3000));
  return r;
}

// Running for `elapsed_ms` at the frozen wall clock (the caller holds a FrozenWallClock).
inline std::unique_ptr<Rig> stopwatch_running_rig(long long elapsed_ms, bool seconds_below = false) {
  auto r = std::make_unique<Rig>();
  const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(ld::wall_now().time_since_epoch()).count();
  r->ctx.state = {{"running", true}, {"started_ms", now - elapsed_ms}, {"accumulated_ms", 0}};
  auto info = ld::stopwatch_module_info();
  r->start(info, info.create(), {{"seconds_below", seconds_below}});
  return r;
}

// What a key module shows now (it must be a KeyModule).
inline std::vector<ld::KeyElement> content_of(ld::Module& m) { return dynamic_cast<ld::KeyModule&>(m).content(); }
inline std::optional<ld::KeyElement> element_of(ld::Module& m, std::string_view key) {
  for (ld::KeyElement& e : content_of(m))
    if (e.key == key) return e;
  return std::nullopt;
}

}  // namespace fixtures
