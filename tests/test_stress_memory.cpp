// Manual memory stress tests (skipped by default). They drive the real render, encode, deck and store
// code paths in a loop and print the process private bytes, to tell a cache that plateaus from a leak.
// Run one with:   lite-deck-tests.exe -tc="stress: <name>" --no-skip   (names avoid commas: doctest splits on them)
// LD_STRESS_FRAMES sets the iteration count (default 30000, about 17 minutes of 30 fps animation).
#include <doctest/doctest.h>
#include <windows.h>
#include <psapi.h>
#include <dwrite_2.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include "core/config_service.h"
#include "core/event_loop.h"
#include "core/strings.h"
#include "fake_loop.h"
#include "layout/deck.h"
#include "device/registry.h"
#include "render/jpeg.h"
#include "modules/common/marquee.h"
#include "modules/notifications/notification_store.h"
#include "modules/notifications/notifications.h"
#include "modules/now_playing/now_playing.h"
#include "render/renderer.h"
#include "web/httplib_wrap.h"
#include "web/web_ui.h"
#include "web_test_util.h"

#pragma comment(lib, "psapi.lib")

using namespace ld;
using nlohmann::json;

static std::wstring emoji_text(int k);

namespace {

double private_mb() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

// Bytes in use (busy blocks) and committed across every heap of the process: tells live allocations
// from memory the heaps keep committed after frees.
std::pair<double, double> heap_mb() {
  std::vector<HANDLE> heaps(256);
  DWORD count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
  double busy = 0, committed = 0;
  for (DWORD h = 0; h < std::min<DWORD>(count, 256); ++h) {
    if (!HeapLock(heaps[h])) continue;
    PROCESS_HEAP_ENTRY e{};
    while (HeapWalk(heaps[h], &e)) {
      if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) busy += e.cbData;
      if (e.wFlags & PROCESS_HEAP_REGION) committed += e.Region.dwCommittedSize;
    }
    HeapUnlock(heaps[h]);
  }
  return {busy / (1024.0 * 1024.0), committed / (1024.0 * 1024.0)};
}

int env_int(const char* name, int fallback) {
  char buf[32]{};
  std::size_t n = 0;
  if (getenv_s(&n, buf, name) == 0 && n > 1) return std::atoi(buf);
  return fallback;
}

int frames() { return env_int("LD_STRESS_FRAMES", 30000); }

struct Meter {
  const char* name;
  int every;
  double start = private_mb();
  double peak = start;
  explicit Meter(const char* n, int total) : name(n), every(std::max(1, total / 20)) {
    std::printf("[%s] start %.2f MB\n", name, start);
  }
  void tick(int i) {
    if (i % every != 0) return;
    double now = private_mb();
    peak = std::max(peak, now);
    std::printf("[%s] %7d  %.2f MB  (%+.2f)\n", name, i, now, now - start);
    std::fflush(stdout);
  }
};

// Titles mixing ASCII, accents, emoji and CJK, like real media titles and toasts.
const std::vector<std::wstring>& texts() {
  static const std::vector<std::wstring> t = {
      L"Daft Punk - Harder, Better, Faster, Stronger (Alive 2007 version, extended)",
      L"Édith Piaf - Non, je ne regrette rien · Hôtel Château Élysée",
      L"🎵 Lo-fi beats to relax/study to 🎧☕ 24/7 live radio 🌙✨",
      L"米津玄師 - Lemon（レモン）· 打上花火 · 感電 · KICK BACK",
      L"BTS (방탄소년단) - Dynamite · 봄날 · Butter 🧈",
      L"Discord: @someone mentioned you in #général - « up for a game tonight? » 🎮",
      L"Outlook: Meeting moved to 2:30 pm - Ångström room, agenda attached 📎",
      L"Сергей Рахманинов - Концерт для фортепиано с оркестром № 2 🎹",
  };
  return t;
}

void draw_strip_like(Canvas& c, const std::wstring& title, const std::wstring& artist, float off) {
  c.clear(Rgb{0, 0, 0});
  TextStyle t;
  t.size = 32;
  t.weight = 600;
  t.align = HAlign::Left;
  TextStyle a = t;
  a.size = 26;
  a.weight = 400;
  draw_marquee(c, title, RectF{50, 16, 340, 48}, t, off, MarqueeOptions{Rgb{0, 0, 0}});
  draw_marquee(c, artist, RectF{50, 60, 340, 40}, a, off, MarqueeOptions{Rgb{0, 0, 0}});
}

// ---- fake sources driven by the test ----

struct FakeNowPlaying final : NowPlayingSource {
  static inline std::function<void(std::optional<Track>)> cb;
  void start(std::function<void(std::optional<Track>)> on_change) override { cb = std::move(on_change); }
};

struct FakeNotifications final : NotificationSource {
  static inline std::vector<IncomingNotification> list;
  void request_access(std::function<void(NotifAccess)> done) override { done(NotifAccess::Allowed); }
  void check_access(std::function<void(NotifAccess)> done) override { done(NotifAccess::Allowed); }
  void fetch(std::function<void(std::optional<std::vector<IncomingNotification>>)> done) override { done(list); }
  void remove(std::uint32_t) override {}
};

struct NullSink : DeckSink {
  std::size_t bytes = 0;
  JpegEncoder jpeg;  // the N4 Pro driver's encode: the measure stays comparable to before
  void key_image(int, const Image& i) override { bytes += jpeg.encode(i, 85, Orientation::Rotate180).size(); }
  void strip_image(const Image& i) override { bytes += jpeg.encode(i, 80, Orientation::Rotate180).size(); }
  void rings(std::span<const Rgb>, uint8_t) override {}
};

}  // namespace

TEST_CASE("stress: jpeg encode of the strip" * doctest::skip()) {
  Renderer r;
  Surface s(r, 800, 112);
  const int n = frames();
  Meter m("jpeg", n);
  std::size_t total = 0;
  for (int i = 0; i <= n; ++i) {
    {
      Canvas c = s.canvas();
      c.clear(Rgb{static_cast<std::uint8_t>(i), 0, 0});
    }
    total += s.encode_jpeg(80, true).size();
    m.tick(i);
  }
  CHECK(total > 0);
}

TEST_CASE("stress: strip text fixed" * doctest::skip()) {
  Renderer r;
  Surface s(r, 800, 112);
  const int n = frames();
  Meter m("text-fixed", n);
  for (int i = 0; i <= n; ++i) {
    {
      Canvas c = s.canvas(RectF{0, 0, 400, 112});
      draw_strip_like(c, texts()[0], texts()[1], i * 2.f);
    }
    s.encode_jpeg(80, true);
    m.tick(i);
  }
}

TEST_CASE("stress: strip text varied" * doctest::skip()) {
  Renderer r;
  Surface s(r, 800, 112);
  const int n = frames();
  Meter m("text-varied", n);
  const int only = env_int("LD_STRESS_TEXT", -1);  // >= 0: a single text of the pool (isolates one script)
  for (int i = 0; i <= n; ++i) {
    const auto& T = texts();
    // A new "track" every 300 frames (10 s), with a unique suffix so no string repeats.
    int k = i / 300;
    std::wstring title = T[only >= 0 ? only : k % T.size()] + L" #" + std::to_wstring(k);
    std::wstring artist = only >= 0 ? L"artist" : T[(k + 3) % T.size()];
    {
      Canvas c = s.canvas(RectF{0, 0, 400, 112});
      draw_strip_like(c, title, artist, static_cast<float>(i % 300) * 2.f);
    }
    s.encode_jpeg(80, true);
    m.tick(i);
  }
}

TEST_CASE("stress: strip text new glyphs" * doctest::skip()) {
  Renderer r;
  Surface s(r, 800, 112);
  const int n = frames();
  Meter m("text-newglyphs", n);
  for (int i = 0; i <= n; ++i) {
    // 12 new CJK ideographs every 30 frames: walks the whole U+4E00 block over the run.
    int k = i / 30;
    std::wstring title;
    for (int j = 0; j < 12; ++j) title.push_back(static_cast<wchar_t>(0x4E00 + (k * 12 + j) % 20992));
    {
      Canvas c = s.canvas(RectF{0, 0, 400, 112});
      draw_strip_like(c, title, L"artist", static_cast<float>(i % 30) * 2.f);
    }
    s.encode_jpeg(80, true);
    m.tick(i);
  }
}

TEST_CASE("stress: text formats with animated sizes" * doctest::skip()) {
  Renderer r;
  Surface s(r, 800, 112);
  const int n = frames();
  Meter m("formats", n);
  for (int i = 0; i <= n; ++i) {
    TextStyle st;
    st.size = 22.f * (1 + 0.25f * static_cast<float>(i % 60) / 60.f);  // the notification pill swelling
    st.weight = 600;
    {
      Canvas c = s.canvas();
      c.clear(Rgb{0, 0, 0});
      c.text(L"12", RectF{700, 10, 80, 40}, st);
    }
    s.encode_jpeg(80, true);
    m.tick(i);
  }
  CHECK(r.cached_formats() <= Renderer::kMaxFormats);
}

TEST_CASE("stress: deck with now_playing and notifications zones" * doctest::skip()) {
  FakeLoop loop;
  Renderer renderer;
  NullSink sink;
  ModuleInfo np = now_playing_module_info();
  np.create = [] { return make_now_playing_module([] { return std::make_unique<FakeNowPlaying>(); }); };
  ModuleInfo nt = notifications_module_info();
  nt.create = [] { return make_notifications_module([] { return std::make_unique<FakeNotifications>(); }); };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* {
    return id == "now_playing" ? &np : id == "notifications" ? &nt : find_module(id);
  };
  Config cfg;
  cfg.strip = {ZoneConfig{400, SlotConfig{"now_playing", {{"scroll_speed", 2}, {"color", "#ffffff"}}}},
               ZoneConfig{400, SlotConfig{"notifications",
                                          {{"scroll_speed", 2}, {"text_color", "#ffffff"}, {"bg_color", "#000000"}}}}};
  FakeNotifications::list.clear();
  Deck deck(loop, renderer, sink, default_device().layout);
  deck.apply(cfg, lookup);
  loop.drain();

  const int n = frames();
  Meter m("deck", n);
  const bool emoji = env_int("LD_STRESS_EMOJI", 0) != 0;
  std::uint32_t next_id = 1;
  for (int i = 0; i <= n; ++i) {
    const auto& T = texts();
    if (i % 1800 == 0) {  // a new track every minute
      int k = i / 1800;
      // LD_STRESS_EMOJI=1: every track and toast also brings 4 emoji never seen before.
      std::wstring extra = emoji ? L" " + emoji_text(2 * k) : L"";
      FakeNowPlaying::cb(Track{T[k % T.size()] + L" #" + std::to_wstring(k) + extra, T[(k + 2) % T.size()], true, 0, 200});
    }
    if (i % 900 == 0) {  // a toast every 30 s, at most 5 kept
      int k = i / 900;
      FakeNotifications::list.push_back(IncomingNotification{next_id++, L"App " + std::to_wstring(k % 4),
                                                             T[(k + 5) % T.size()],
                                                             T[(k + 1) % T.size()] + (emoji ? emoji_text(k + 96) : L""), {}});
      if (FakeNotifications::list.size() > 5) FakeNotifications::list.erase(FakeNotifications::list.begin());
    }
    loop.advance(Deck::kFrame);
    m.tick(i);
  }
  CHECK(sink.bytes > 0);
}

TEST_CASE("stress: notification store churn" * doctest::skip()) {
  NotificationStore store;
  const int n = frames() * 10;
  Meter m("store", n);
  std::vector<IncomingNotification> list;
  std::uint32_t next = 1;
  for (int i = 0; i <= n; ++i) {
    if (i % 3 == 0) list.push_back(IncomingNotification{next++, L"app", L"title " + std::to_wstring(i), L"body", {}});
    if (list.size() > 8) list.erase(list.begin());
    store.sync(list, TimePoint{});
    if (i % 7 == 0)
      if (auto c = store.current()) store.remove(c->id);
    m.tick(i);
  }
}

// Read-only: lists the real toasts and reads the media session, never removes or controls anything.
TEST_CASE("stress: real WinRT notification fetch (read-only)" * doctest::skip()) {
  auto src = make_winrt_notification_source();
  std::promise<NotifAccess> access;
  src->request_access([&](NotifAccess a) { access.set_value(a); });
  NotifAccess a = access.get_future().get();
  std::printf("[winrt-notif] access %d\n", static_cast<int>(a));
  if (a != NotifAccess::Allowed) return;
  const int n = frames() / 10;
  Meter m("winrt-notif", n);
  for (int i = 0; i <= n; ++i) {
    std::promise<std::size_t> p;
    src->fetch([&](std::optional<std::vector<IncomingNotification>> l) { p.set_value(l ? l->size() : 0); });
    std::size_t count = p.get_future().get();
    if (i == 0) std::printf("[winrt-notif] %zu toasts listed\n", count);
    m.tick(i);
  }
}

TEST_CASE("stress: real GSMTC source start/stop (read-only)" * doctest::skip()) {
  const int n = frames() / 100;
  Meter m("gsmtc", n);
  for (int i = 0; i <= n; ++i) {
    std::promise<void> first;
    bool got = false;
    std::mutex mu;
    {
      auto src = make_gsmtc_source();
      src->start([&](std::optional<Track>) {
        std::lock_guard lock(mu);
        if (!got) {
          got = true;
          first.set_value();
        }
      });
      first.get_future().wait_for(std::chrono::seconds(2));
    }
    m.tick(i);
  }
}

// "Now " + 4 emoji, all new for each k (walks U+1F300-1F5FF, wraps after 192 steps).
static std::wstring emoji_text(int k) {
  std::wstring text = L"Now ";
  for (int j = 0; j < 4; ++j) {
    char32_t cp = 0x1F300 + static_cast<char32_t>((k * 4 + j) % 0x300) - 0x10000;
    text.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
    text.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
  }
  return text;
}

// Splits the cost of an emoji: LD_STRESS_MODE 0 = layout + metrics only, 1 = draw without color fonts,
// 2 = draw with D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT (what Canvas::text does).
TEST_CASE("stress: emoji cost split" * doctest::skip()) {
  Renderer r;
  ComPtr<IWICBitmap> bmp;
  REQUIRE(SUCCEEDED(r.wic()->CreateBitmap(800, 112, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)));
  ComPtr<ID2D1RenderTarget> rt;
  auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  REQUIRE(SUCCEEDED(r.d2d()->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt)));
  ComPtr<ID2D1SolidColorBrush> brush;
  rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
  const int mode = env_int("LD_STRESS_MODE", 2);
  const int n = frames();
  Meter m(mode == 0 ? "emoji-layout" : mode == 1 ? "emoji-mono" : "emoji-color", n);
  TextStyle st;
  st.size = 32;
  const bool walk = env_int("LD_STRESS_WALK", 0) != 0;  // ever-new emoji instead of the same three
  for (int i = 0; i <= n; ++i) {
    std::wstring text = L"🎵 Lo-fi 🎧☕ #" + std::to_wstring(i / 300);
    if (walk) text = emoji_text(i / 30);
    ComPtr<IDWriteTextLayout> l;
    r.dwrite()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), r.text_format(st), 800, 112, &l);
    DWRITE_TEXT_METRICS tm{};
    l->GetMetrics(&tm);
    if (mode > 0) {
      rt->BeginDraw();
      rt->Clear(D2D1::ColorF(0, 0, 0));
      rt->DrawTextLayout(D2D1::Point2F(0, 0), l.Get(), brush.Get(),
                         mode == 2 ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT : D2D1_DRAW_TEXT_OPTIONS_NONE);
      rt->EndDraw();
    }
    m.tick(i);
  }
}

// Ever-new color emoji (4 new ones every 30 frames, walking U+1F300-1F5FF then wrapping): does the
// color glyph cost grow per glyph without bound, or is it trimmed? LD_STRESS_SURFACES spreads the
// draws over that many surfaces (render targets).
TEST_CASE("stress: emoji new glyphs" * doctest::skip()) {
  Renderer r;
  std::vector<std::unique_ptr<Surface>> surfaces;
  const int count = std::max(1, env_int("LD_STRESS_SURFACES", 1));
  for (int i = 0; i < count; ++i) surfaces.push_back(std::make_unique<Surface>(r, 800, 112));
  const int n = frames();
  Meter m("emoji-walk", n);
  TextStyle st;
  st.size = 32;
  for (int i = 0; i <= n; ++i) {
    int k = i / 30;
    if (env_int("LD_STRESS_SIZES", 0)) st.size = 24.f + static_cast<float>((k / 192) % 33) * 0.5f;  // a new size per lap
    std::wstring text = emoji_text(k);
    Surface& s = *surfaces[static_cast<std::size_t>(k) % surfaces.size()];
    {
      Canvas c = s.canvas();
      c.clear(Rgb{0, 0, 0});
      c.text(text, RectF{0, 0, 800, 112}, st);
    }
    s.encode_jpeg(80, true);
    m.tick(i);
  }
}

// Is the emoji cost owned by the Renderer (D2D/DWrite factories, render targets)? Walks emoji, drops
// every Renderer object, then measures again.
TEST_CASE("stress: emoji then renderer reset" * doctest::skip()) {
  const int n = frames();
  Meter m("emoji-reset", n);
  {
    Renderer r;
    Surface s(r, 800, 112);
    TextStyle st;
    st.size = 32;
    for (int i = 0; i <= n; ++i) {
      {
        Canvas c = s.canvas();
        c.clear(Rgb{0, 0, 0});
        c.text(emoji_text(i / 30), RectF{0, 0, 800, 112}, st);
      }
      s.encode_jpeg(80, true);
      m.tick(i);
    }
  }
  std::printf("[emoji-reset] after Renderer destroyed: %.2f MB\n", private_mb());
  {
    Renderer r;
    Surface s(r, 800, 112);
    TextStyle st;
    st.size = 32;
    for (int i = 0; i < 300; ++i) {
      Canvas c = s.canvas();
      c.clear(Rgb{0, 0, 0});
      c.text(emoji_text(i / 30), RectF{0, 0, 800, 112}, st);
    }
    s.encode_jpeg(80, true);
  }
  std::printf("[emoji-reset] after a new Renderer drew 40 emoji: %.2f MB\n", private_mb());
}

// Which object owns the color emoji cost? Walks emoji on raw D2D/DWrite objects, then releases the
// render target, the D2D factory and the DWrite factory one by one. LD_STRESS_ISOLATED=1 uses an
// isolated DWrite factory instead of the shared (process-wide) one.
TEST_CASE("stress: emoji cost owner" * doctest::skip()) {
  const bool isolated = env_int("LD_STRESS_ISOLATED", 0) != 0;
  ComPtr<IWICImagingFactory> wic;
  REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
  ComPtr<ID2D1Factory> d2d;
  REQUIRE(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())));
  ComPtr<IDWriteFactory> dw;
  REQUIRE(SUCCEEDED(DWriteCreateFactory(isolated ? DWRITE_FACTORY_TYPE_ISOLATED : DWRITE_FACTORY_TYPE_SHARED,
                                        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))));
  ComPtr<IWICBitmap> bmp;
  REQUIRE(SUCCEEDED(wic->CreateBitmap(800, 112, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)));
  ComPtr<ID2D1RenderTarget> rt;
  auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  REQUIRE(SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt)));
  ComPtr<ID2D1SolidColorBrush> brush;
  rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
  ComPtr<IDWriteTextFormat> fmt;
  dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, 32, L"fr-fr", &fmt);
  const int n = frames();
  Meter m(isolated ? "owner-isolated" : "owner-shared", n);
  for (int i = 0; i <= n; ++i) {
    std::wstring text = emoji_text(i / 30);
    ComPtr<IDWriteTextLayout> l;
    dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.Get(), 800, 112, &l);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0));
    rt->DrawTextLayout(D2D1::Point2F(0, 0), l.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    rt->EndDraw();
    m.tick(i);
  }
  brush.Reset();
  rt.Reset();
  std::printf("[owner] render target released: %.2f MB\n", private_mb());
  bmp.Reset();
  d2d.Reset();
  std::printf("[owner] D2D factory released: %.2f MB\n", private_mb());
  fmt.Reset();
  dw.Reset();
  std::printf("[owner] DWrite factory released: %.2f MB\n", private_mb());
  HeapCompact(GetProcessHeap(), 0);
  std::printf("[owner] after HeapCompact: %.2f MB\n", private_mb());
}

// Does recycling the D2D factory bound the color emoji cost? Each cycle walks LD_STRESS_FRAMES frames of
// new emoji (continuing the walk) on a fresh D2D factory and render target, then releases them.
TEST_CASE("stress: emoji d2d factory cycles" * doctest::skip()) {
  ComPtr<IWICImagingFactory> wic;
  REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
  ComPtr<IDWriteFactory> dw;
  REQUIRE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dw.GetAddressOf()))));
  ComPtr<IDWriteTextFormat> fmt;
  dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, 32, L"fr-fr", &fmt);
  ComPtr<IWICBitmap> bmp;
  REQUIRE(SUCCEEDED(wic->CreateBitmap(800, 112, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)));
  const int n = frames();
  const int cycles = std::max(1, env_int("LD_STRESS_CYCLES", 6));
  auto [busy0, committed0] = heap_mb();
  std::printf("[cycles] start %.2f MB (heaps %.2f busy, %.2f committed)\n", private_mb(), busy0, committed0);
  int frame = 0;
  for (int c = 0; c < cycles; ++c) {
    double peak = 0;
    {
      ComPtr<ID2D1Factory> d2d;
      REQUIRE(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())));
      ComPtr<ID2D1RenderTarget> rt;
      auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      REQUIRE(SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt)));
      ComPtr<ID2D1SolidColorBrush> brush;
      rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
      for (int i = 0; i < n; ++i, ++frame) {
        std::wstring text = emoji_text(frame / 30);
        ComPtr<IDWriteTextLayout> l;
        dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.Get(), 800, 112, &l);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0));
        rt->DrawTextLayout(D2D1::Point2F(0, 0), l.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        rt->EndDraw();
        if (i % 500 == 0) peak = std::max(peak, private_mb());
      }
      peak = std::max(peak, private_mb());
    }
    auto [busy, committed] = heap_mb();
    std::printf("[cycles] cycle %d (emoji %d-%d): peak %.2f MB, after release %.2f MB (heaps %.2f busy, %.2f committed)\n",
                c, ((frame - n) / 30 * 4) % 0x300, (frame / 30 * 4) % 0x300, peak, private_mb(), busy, committed);
    std::fflush(stdout);
  }
}

// Color emoji drawn as COLR v0 layers through a custom IDWriteTextRenderer (LD_STRESS_WALK=1: new emoji).
// Writes the last frame to LD_STRESS_PNG if set, to compare the look.
TEST_CASE("stress: emoji as COLR layers" * doctest::skip()) {
  Renderer r;
  Surface s(r, 800, 112);
  const bool walk = env_int("LD_STRESS_WALK", 1) != 0;
  const int n = frames();
  Meter m("colr-layers", n);
  TextStyle st;
  st.size = 32;
  for (int i = 0; i <= n; ++i) {
    std::wstring t = walk ? emoji_text(i / 30) : std::wstring(L"🎵 Lo-fi 🎧☕");
    {
      Canvas c = s.canvas();
      c.clear(Rgb{0, 0, 0});
      c.text(t, RectF{static_cast<float>(i % 30) * 0.37f, 0, 800, 112}, st);
    }
    m.tick(i);
  }
  char path[260]{};
  std::size_t len = 0;
  if (getenv_s(&len, path, "LD_STRESS_PNG") == 0 && len > 1) {
    {
      Canvas c = s.canvas();
      c.clear(Rgb{0, 0, 0});
      c.text(L"🎵 Lo-fi 🎧☕ 😀🍕🚀", RectF{0, 0, 800, 56}, st);
    }
    auto jpeg = s.encode_jpeg(90, false);
    FILE* fp = nullptr;
    std::string p1 = std::string(path) + "-layers.jpg";
    if (fopen_s(&fp, p1.c_str(), "wb") == 0) { fwrite(jpeg.data(), 1, jpeg.size(), fp); fclose(fp); }
  }
}

// Manual timing of the strip animation (skipped by default): a scrolling now_playing zone next to a
// static notifications zone, as on the device. Prints ms per strip frame (wall and CPU), and the cost
// of the JPEG encode alone, which every frame pays. Run in a Release build:
//   lite-deck-tests.exe -tc="perf: strip frame time" --no-skip     (LD_PERF_FRAMES, default 1800)
namespace {
struct CountingSink : DeckSink {
  int strip_frames = 0;
  JpegEncoder jpeg;
  void key_image(int, const Image&) override {}
  void strip_image(const Image& i) override {
    jpeg.encode(i, 80, Orientation::Rotate180);
    ++strip_frames;
  }
  void rings(std::span<const Rgb>, uint8_t) override {}
};
double cpu_ms() {
  FILETIME c, e, k, u;
  GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
  auto ms = [](FILETIME f) { return static_cast<double>((static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
  return ms(k) + ms(u);
}
}  // namespace

TEST_CASE("perf: strip frame time" * doctest::skip()) {
  FakeLoop loop;
  Renderer renderer;
  CountingSink sink;
  ModuleInfo np = now_playing_module_info();
  np.create = [] { return make_now_playing_module([] { return std::make_unique<FakeNowPlaying>(); }); };
  ModuleInfo nt = notifications_module_info();
  nt.create = [] { return make_notifications_module([] { return std::make_unique<FakeNotifications>(); }); };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* {
    return id == "now_playing" ? &np : id == "notifications" ? &nt : find_module(id);
  };
  Config cfg;
  cfg.strip = {ZoneConfig{400, SlotConfig{"now_playing", {{"scroll_speed", 2}, {"color", "#ffffff"}}}},
               ZoneConfig{400, SlotConfig{"notifications",
                                          {{"scroll_speed", 2}, {"text_color", "#ffffff"}, {"bg_color", "#000000"}}}}};
  FakeNotifications::list = {IncomingNotification{1, L"Discord", L"Alex", L"on joue ce soir ?", {}}};  // fits: static
  Deck deck(loop, renderer, sink, default_device().layout);
  deck.apply(cfg, lookup);
  loop.drain();
  FakeNowPlaying::cb(Track{texts()[0], texts()[1], true, 0, 200});  // too long: scrolls
  loop.drain();
  loop.advance(std::chrono::milliseconds(2000));  // warm-up: caches filled

  const int n = env_int("LD_PERF_FRAMES", 1800);
  const int before = sink.strip_frames;
  const double cpu0 = cpu_ms();
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) loop.advance(Deck::kFrame);
  const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double cpu = cpu_ms() - cpu0;
  const int frames = sink.strip_frames - before;
  REQUIRE(frames > n / 2);

  Surface s(renderer, 800, 112);
  { Canvas c = s.canvas(); draw_strip_like(c, texts()[0], texts()[1], 0); }
  const auto e0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 300; ++i) s.encode_jpeg(80, true);
  const double enc = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count() / 300;
  std::printf("[perf] %d strip frames: %.3f ms/frame wall, %.3f ms/frame cpu; jpeg encode alone %.3f ms; %.2f MB private\n",
              frames, wall / frames, cpu / frames, enc, private_mb());
}

// The web UI's cost (spec §7): with no page open, at most 0.5 MB and no work per frame; with a page
// open (previews refreshed as fast as an animated deck asks, a save every 10 rounds), at most 1.5 MB
// more, given back once the page closes. The "page" is a raw socket client with fixed buffers, so
// that only the daemon side is measured. LD_STRESS_WEB_ROUNDS sets the rounds (default 2000).
// Run it on a Release build.
namespace {

// One request on a new connection (the server serves one per connection); the answer is read into
// `buf` and dropped. Returns the status (0 = no answer).
int raw_request(int port, const std::string& request, std::vector<char>& buf) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  int status = 0;
  if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
      send(s, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size())) {
    int got = 0, n = 0;
    while ((n = recv(s, buf.data() + got, static_cast<int>(buf.size()) - got, 0)) > 0) {
      if (got < 12) got += n;  // keeps the status line, overwrites the rest
    }
    if (got >= 12) status = std::atoi(buf.data() + 9);
  }
  closesocket(s);
  return status;
}

std::string raw_get(int port, const std::string& path, const std::string& cookie) {
  return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nCookie: " + cookie + "\r\n\r\n";
}

}  // namespace

TEST_CASE("stress: web ui page closed then open" * doctest::skip()) {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  const fs::path dir = fs::temp_directory_path() / "ld_stress_web";
  fs::remove_all(dir);
  fs::create_directories(dir);
  testutil::write_text(dir / "config.json", testutil::doc_with_city("Brussels").dump());
  EventLoop loop;
  Renderer renderer;
  NullSink sink;
  ConfigService config(dir / "config.json", testutil::city_lookup);
  Deck deck(loop, renderer, sink, default_device().layout);
  deck.apply(config.load(), testutil::city_lookup);
  {  // one-time costs that are not the page's: Winsock, the first server, the deck's first frames
    WebUi warm(loop, config, deck, [] { return std::string("simulator"); });
    REQUIRE(warm.start(0));
    loop.at(loop.now() + 1s, [&] { loop.quit(0); });
    loop.run();
  }
  const double before = private_mb();
  const double before_live = heap_mb().first;
  const int rounds = env_int("LD_STRESS_WEB_ROUNDS", 2000);
  double closed = 0, open_peak = 0, after = 0, closed_live = 0, after_live = 0;
  std::atomic<int> phase{0};  // 0 no page, 1 page open, 2 page closed again, 3 done
  {
    WebUi web(loop, config, deck, [] { return std::string("simulator"); });
    REQUIRE(web.start(0));
    const std::string url = narrow(web.config_url());
    const int port = std::stoi(url.substr(17, url.find('/', 17) - 17));
    const std::string cookie = "ld=" + url.substr(url.find("?t=") + 3);
    const int keys = default_device().layout.key_count();
    // An animated deck: every target has a new frame every 33 ms.
    std::function<void()> animate = [&] {
      for (int t = -1; t < keys; ++t) web.frame(t);
      if (phase == 1) open_peak = std::max(open_peak, private_mb());
      if (phase < 3) loop.at(loop.now() + 33ms, animate);
    };
    loop.at(loop.now() + 33ms, animate);
    std::thread client([&] {
      std::vector<char> buf(256 * 1024);
      std::this_thread::sleep_for(2s);  // frames with no page: nothing may grow
      closed = private_mb();
      closed_live = heap_mb().first;
      std::atomic<bool> leave{false};
      std::thread page([&] {  // the event stream, read and dropped until the page leaves
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        std::vector<char> in(4096);
        if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) {
          const std::string req = raw_get(port, "/api/events", cookie);
          send(s, req.data(), static_cast<int>(req.size()), 0);
          while (!leave && recv(s, in.data(), static_cast<int>(in.size()), 0) > 0) {
          }
        }
        closesocket(s);
      });
      phase = 1;
      std::vector<std::string> previews;
      for (int k = 0; k < keys; ++k) previews.push_back(raw_get(port, "/api/preview/key/" + std::to_string(k), cookie));
      previews.push_back(raw_get(port, "/api/preview/strip/0", cookie));
      const std::string body_a = testutil::doc_with_city("Liège").dump();
      const std::string body_b = testutil::doc_with_city("Namur").dump();
      auto put = [&](const std::string& body) {
        return "PUT /api/config HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nCookie: " + cookie +
               "\r\nX-Lite-Deck: 1\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\n\r\n" + body;
      };
      const std::string put_a = put(body_a), put_b = put(body_b);
      int bad = 0;
      for (int i = 0; i < rounds; ++i) {
        for (const std::string& r : previews) bad += raw_request(port, r, buf) != 200;
        if (i % 10 == 0) bad += raw_request(port, (i / 10) % 2 ? put_a : put_b, buf) != 200;
        if (i % (rounds / 10 + 1) == 0) std::printf("[web] round %d  %.2f MB\n", i, private_mb());
      }
      std::printf("[web] %d requests not answered 200\n", bad);
      leave = true;
      page.join();
      phase = 2;
      std::this_thread::sleep_for(10s);  // the stream ends at the next ping; the cache goes with it
      after = private_mb();
      after_live = heap_mb().first;
      phase = 3;
      loop.quit(0);
    });
    loop.run();
    client.join();
    CHECK_FALSE(web.preview_stats().encoder);
    CHECK(web.preview_stats().bytes == 0);
  }
  std::printf("[web] private: before %.2f, page closed %.2f (+%.2f), page open peak %.2f (+%.2f), closed again %.2f;"
              " live heap: before %.2f, page closed %.2f, closed again %.2f\n", before, closed, closed - before, open_peak,
              open_peak - closed, after, before_live, closed_live, after_live);
  // Private bytes move by a few hundred KB from run to run (heap commits); the live heap does not.
  CHECK(closed_live - before_live <= 0.5);  // the web part with no page open
  CHECK(open_peak - closed <= 1.5);         // a page open
  CHECK(after_live - closed_live <= 0.25);  // given back: what stays committed is heap slack, not data
  std::error_code ec;
  fs::remove_all(dir, ec);
}
