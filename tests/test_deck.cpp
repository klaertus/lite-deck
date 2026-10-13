#include <doctest/doctest.h>
#include <algorithm>
#include <shlwapi.h>
#include <wincodec.h>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <fstream>
#include "core/log.h"
#include "core/state_store.h"
#include "fake_device.h"
#include "fake_loop.h"
#include "layout/deck.h"
#include "device/registry.h"
#include "render/jpeg.h"

#pragma comment(lib, "shlwapi.lib")

using namespace ld;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Probe {
  std::vector<std::pair<int, EventKind>> events;  // (instance id, kind)
  std::vector<int> xs;
  std::vector<int> deltas;
  int renders = 0;
  std::map<int, int> renders_of;  // instance id -> render count
  struct Seen {
    std::wstring font;
    bool italic = false;
    float size = 0;
    Rgb stale{};
    float value_size = 0;    // kProbeValue, a Value-role text
    std::wstring value_font; // its TextStyle font: "" = the slot's
  };
  std::map<int, Seen> last;  // instance id -> what its last render saw
};

constexpr ElementSpec kProbeElement{"label", "Label", 20, 400, 8, 60};
constexpr ElementSpec kProbeValue{.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10,
                                  .max_size = 80, .role = TextRole::Value};

struct TestModule : Module {
  Probe* probe;
  int id;
  ModuleContext* ctx = nullptr;
  Poster poster;
  bool throw_render = false, throw_event = false, anim = false, slow = false;
  bool text = false;   // draws text that moves with the clock and runs past the zone's right edge
  bool once = false;   // paints its first frame only, then draws nothing

  TestModule(Probe* p, int i) : probe(p), id(i) {}
  void start(ModuleContext& c, const json& s) override {
    ctx = &c;
    poster = c.poster();
    throw_render = s.value("throw_render", false);
    throw_event = s.value("throw_event", false);
    anim = s.value("anim", false);
    slow = s.value("slow", false);
    text = s.value("text", false);
    once = s.value("once", false);
    if (s.value("ring", false)) c.set_ring_color(Rgb{255, 0, 0});
    if (s.value("ring_black", false)) c.set_ring_color(Rgb{0, 0, 0});
  }
  void on_event(const InputEvent& e) override {
    if (throw_event) throw std::runtime_error("boom");
    probe->events.push_back({id, e.kind});
    probe->xs.push_back(e.x);
    probe->deltas.push_back(e.delta);
  }
  void render(Canvas& c) override {
    if (throw_render) throw std::runtime_error("boom");
    ++probe->renders;
    ++probe->renders_of[id];
    const TextStyle value = c.style(kProbeValue);
    probe->last[id] = Probe::Seen{c.look().font, c.look().italic, c.style(kProbeElement).size, c.look().states.stale,
                                  value.size, value.font};
    if (once && probe->renders_of[id] > 1) return;
    if (text) {
      const float t = static_cast<float>(std::chrono::duration_cast<Duration>(ctx->now().time_since_epoch()).count() % 1000);
      TextStyle st;
      st.size = 30;
      st.align = HAlign::Left;
      st.color = Rgb{255, 255, 255};
      c.text(L"Probe text ✓ " + std::to_wstring(id), RectF{c.width() - 150 - t / 20, 10, 400, 90}, st);
      return;
    }
    c.clear(Rgb{0, 200, 0});
  }
  bool animating() const override { return anim; }
  bool slow_animation() const override { return slow; }
  void fetch() {
    ctx->http_get("https://x", [this](const HttpResponse& r) {
      probe->events.push_back({id, EventKind::Tap});
      probe->xs.push_back(r.status);
    });
  }
};

struct FakeHttp : HttpClient {
  std::vector<std::function<void(HttpResponse)>> pending;
  std::vector<HttpRequest> requests;
  std::vector<std::function<bool()>> wanted;
  void get(const HttpRequest& r, std::function<void(HttpResponse)> done, std::function<bool()> w) override {
    requests.push_back(r);
    pending.push_back(std::move(done));
    wanted.push_back(std::move(w));
  }
};

struct RecordingSink : DeckSink {
  std::map<int, int> key_frames;
  int strip_frames = 0;
  std::vector<std::vector<Rgb>> ring_calls;
  std::vector<uint8_t> ring_levels;
  std::vector<std::pair<int, int>> key_sizes;  // (width, height) of each key frame
  std::pair<int, int> last_strip{};
  void key_image(int slot, const Image& image) override {
    CHECK(image.bgra.size() >= static_cast<std::size_t>(image.stride) * (image.height - 1) + image.width * 4u);
    key_sizes.emplace_back(image.width, image.height);
    ++key_frames[slot];
  }
  void strip_image(const Image& image) override {
    last_strip = {image.width, image.height};
    ++strip_frames;
  }
  void rings(std::span<const Rgb> c, uint8_t level) override {
    ring_calls.emplace_back(c.begin(), c.end());
    ring_levels.push_back(level);
  }
};

json loose_schema() {
  return json::parse(R"({"fields":[
    {"key":"throw_render","type":"bool","default":false,"label":"x"},
    {"key":"throw_event","type":"bool","default":false,"label":"x"},
    {"key":"anim","type":"bool","default":false,"label":"x"},
    {"key":"slow","type":"bool","default":false,"label":"x"},
    {"key":"text","type":"bool","default":false,"label":"x"},
    {"key":"once","type":"bool","default":false,"label":"x"},
    {"key":"ring","type":"bool","default":false,"label":"x"},
    {"key":"ring_black","type":"bool","default":false,"label":"x"}]})");
}

struct Harness {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  Probe probe;
  int next_id = 0;
  std::vector<TestModule*> created;
  ModuleInfo info;
  ModuleLookup lookup;
  Deck deck;
  explicit Harness(const DeviceLayout& layout = default_device().layout) : deck(loop, renderer, sink, layout) {
    info.id = "test";
    info.name = "test";
    info.slots = {SlotKind::Key, SlotKind::Knob, SlotKind::StripZone};
    info.settings_schema = loose_schema();
    info.create = [this] {
      auto m = std::make_unique<TestModule>(&probe, next_id++);
      created.push_back(m.get());
      return m;
    };
    lookup = [this](std::string_view id) -> const ModuleInfo* { return id == "test" ? &info : nullptr; };
  }
  static SlotConfig slot(json settings = json::object()) {
    json full = {{"throw_render", false}, {"throw_event", false}, {"anim", false}, {"slow", false}, {"ring", false},
                 {"text", false}, {"once", false}, {"ring_black", false}};
    full.update(settings);
    return SlotConfig{"test", full};
  }
};

DeviceInput raw(InputKind k, int index = 0, int delta = 0, int x = 0) {
  return DeviceInput{k, index, delta, x, 0};
}

}  // namespace

TEST_CASE("apply renders every key and the strip once") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.sink.key_frames.size() == 10);  // empty keys are drawn black too
  CHECK(h.sink.strip_frames == 1);
  CHECK(h.probe.renders == 1);
  REQUIRE(h.sink.ring_calls.size() == 1);
  CHECK(h.sink.ring_calls[0] == std::vector<Rgb>(c.ring_colors.begin(), c.ring_colors.end()));
}

TEST_CASE("key press, release and tap; long press suppresses the tap") {
  Harness h;
  Config c;
  c.keys[3] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  h.deck.on_input(raw(InputKind::KeyDown, 3));
  h.loop.advance(100ms);
  h.deck.on_input(raw(InputKind::KeyUp, 3));
  CHECK(h.probe.events == std::vector<std::pair<int, EventKind>>{{0, EventKind::Press}, {0, EventKind::Release}, {0, EventKind::Tap}});
  h.probe.events.clear();
  h.deck.on_input(raw(InputKind::KeyDown, 3));
  h.loop.advance(600ms);
  h.deck.on_input(raw(InputKind::KeyUp, 3));
  CHECK(h.probe.events == std::vector<std::pair<int, EventKind>>{{0, EventKind::Press}, {0, EventKind::LongPress}, {0, EventKind::Release}});
}

TEST_CASE("knob turns and presses reach the knob module") {
  Harness h;
  Config c;
  c.knobs[2] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.deck.on_input(raw(InputKind::KnobTurn, 2, +1));
  h.deck.on_input(raw(InputKind::KnobPress, 2));
  CHECK(h.probe.events == std::vector<std::pair<int, EventKind>>{{0, EventKind::Rotate}, {0, EventKind::Press}});
}

TEST_CASE("strip gestures go to the zone under the start point with zone-relative x") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{430, Harness::slot()}, ZoneConfig{370, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.deck.on_input(raw(InputKind::TouchDown, 0, 0, 500));
  h.deck.on_input(raw(InputKind::TouchMove, 0, 0, 502));
  h.deck.on_input(raw(InputKind::TouchUp));
  REQUIRE(h.probe.events.size() == 1);
  CHECK(h.probe.events[0] == std::pair<int, EventKind>{1, EventKind::Tap});
  CHECK(h.probe.xs[0] == 70);
}

TEST_CASE("Deck isolates a throwing module") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot({{"throw_render", true}});
  c.keys[1] = Harness::slot({{"throw_event", true}});
  c.keys[2] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.sink.key_frames[0] == 1);  // render threw: "!" drawn in the same pass
  CHECK(h.sink.key_frames[1] == 1);
  h.deck.on_input(raw(InputKind::KeyDown, 1));  // on_event throws
  h.loop.drain();
  CHECK(h.sink.key_frames[1] == 2);  // re-rendered as "!"
  h.deck.on_input(raw(InputKind::KeyDown, 2));
  CHECK(h.probe.events.back() == std::pair<int, EventKind>{2, EventKind::Press});
  h.deck.redraw_all();
  h.loop.drain();
  CHECK(h.probe.renders == 2 + 1);  // keys 1 and 2 rendered once at start, key 2 again; key 0 never
}

TEST_CASE("animating modules are re-rendered at about 30 fps, others are not") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{800, Harness::slot({{"anim", true}})}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  int before = h.sink.strip_frames;
  h.loop.advance(1000ms);
  CHECK(h.sink.strip_frames - before >= 28);
  CHECK(h.sink.strip_frames - before <= 31);
}

TEST_CASE("slow animations run at about 20 fps alone, 30 fps next to a fast one") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot({{"anim", true}, {"slow", true}});
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  int before = h.sink.key_frames[0];
  h.loop.advance(1000ms);
  CHECK(h.sink.key_frames[0] - before >= 19);
  CHECK(h.sink.key_frames[0] - before <= 21);
  c.strip = {ZoneConfig{800, Harness::slot({{"anim", true}})}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  before = h.sink.key_frames[0];
  int strip_before = h.sink.strip_frames;
  h.loop.advance(1000ms);
  CHECK(h.sink.key_frames[0] - before >= 28);
  CHECK(h.sink.strip_frames - strip_before >= 28);
}

TEST_CASE("the frame period follows slow_animation changes while the timer runs") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot({{"anim", true}});
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 1);
  TestModule& m = *h.created[0];
  auto frames_in_1s = [&] {
    int before = h.sink.key_frames[0];
    h.loop.advance(1000ms);
    return h.sink.key_frames[0] - before;
  };
  int fast = frames_in_1s();
  CHECK(fast >= 28);
  CHECK(fast <= 31);
  m.slow = true;  // no apply(): the deck notices on its next render pass
  m.ctx->invalidate();
  h.loop.drain();
  frames_in_1s();  // the switch happens during this window
  int slow = frames_in_1s();
  CHECK(slow >= 19);
  CHECK(slow <= 21);
  m.slow = false;
  m.ctx->invalidate();
  h.loop.drain();
  frames_in_1s();
  int fast_again = frames_in_1s();
  CHECK(fast_again >= 28);
  CHECK(fast_again <= 31);
  CHECK(h.loop.sched.size() == 1);  // one animation timer, not one per switch
}

TEST_CASE("idle deck does no work") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  int renders = h.probe.renders;
  h.loop.advance(5000ms);
  CHECK(h.probe.renders == renders);
  CHECK(h.loop.sched.size() == 0);
}

TEST_CASE("ring overrides from knob modules and suspend/resume") {
  Harness h;
  Config c;
  c.knobs[1] = Harness::slot({{"ring", true}});
  h.deck.apply(c, h.lookup);
  REQUIRE_FALSE(h.sink.ring_calls.empty());
  CHECK(h.sink.ring_calls.back()[1] == Rgb{255, 0, 0});
  CHECK(h.sink.ring_calls.back()[0] == c.ring_colors[0]);
  h.deck.suspend();
  h.deck.on_input(raw(InputKind::KnobPress, 1));
  int count = static_cast<int>(h.probe.events.size());
  CHECK(count == 0);
  h.deck.resume();
  h.loop.drain();
  h.deck.on_input(raw(InputKind::KnobPress, 1));
  CHECK(h.probe.events.size() == 1);
}

namespace {
struct DtorModule : Module {
  ModuleContext* ctx = nullptr;
  void start(ModuleContext& c, const json&) override {
    ctx = &c;
    c.set_ring_color(Rgb{1, 2, 3});
  }
  void on_event(const InputEvent&) override {}
  void render(Canvas&) override {}
  ~DtorModule() override {
    ctx->clear_ring_color();
    ctx->set_ring_color(Rgb{9, 9, 9});
    ctx->invalidate();
  }
};

struct ThrowingSink : RecordingSink {
  void key_image(int slot, const Image& image) override {
    if (slot == 4) throw std::runtime_error("sink");
    RecordingSink::key_image(slot, image);
  }
};
}  // namespace

TEST_CASE("module destructors may call back into the context during teardown") {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  ModuleInfo info;
  info.id = "dtor";
  info.name = "dtor";
  info.slots = {SlotKind::Knob};
  info.settings_schema = json::parse(R"({"fields":[]})");
  info.create = [] { return std::make_unique<DtorModule>(); };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* { return id == "dtor" ? &info : nullptr; };
  {
    Deck deck{loop, renderer, sink, default_device().layout};
    Config c;
    for (auto& k : c.knobs) k = SlotConfig{"dtor", json::object()};
    deck.apply(c, lookup);
    loop.drain();
    deck.apply(Config{}, lookup);
    loop.drain();
    deck.apply(c, lookup);
  }
  loop.drain();
}

TEST_CASE("a failing frame is dropped without stopping the pass") {
  Harness h;
  ThrowingSink ts;
  Deck deck{h.loop, h.renderer, ts, default_device().layout};
  Config c;
  c.keys[0] = Harness::slot();
  deck.apply(c, h.lookup);
  CHECK_NOTHROW(h.loop.drain());
  CHECK(ts.key_frames.count(4) == 0);
  CHECK(ts.key_frames.size() == 9);
  CHECK(ts.strip_frames == 1);
}

TEST_CASE("a module that fails to start shows the error glyph") {
  Harness h;
  h.info.create = []() -> std::unique_ptr<Module> { throw std::runtime_error("create"); };
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.sink.key_frames[0] == 1);
  CHECK(h.sink.ring_calls.size() == 1);
}

TEST_CASE("a poster used from another thread runs on the loop while the module lives") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 1);
  Poster p = h.created[0]->poster;
  int ran = 0;
  std::thread t([&] { p.post([&] { ++ran; }); });
  t.join();
  h.loop.drain();
  CHECK(ran == 1);
}

TEST_CASE("a poster used after the module is destroyed is a no-op") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  Poster p = h.created[0]->poster;
  h.deck.apply(Config{}, h.lookup);  // destroys the module
  h.loop.drain();
  int ran = 0;
  std::thread t([&] { p.post([&] { ++ran; }); });
  t.join();
  h.loop.drain();
  CHECK(ran == 0);
}

TEST_CASE("a touch that spans a reload does not reach the new zones") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{800, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.deck.on_input(raw(InputKind::TouchDown, 0, 0, 100));
  h.deck.apply(c, h.lookup);
  h.deck.on_input(raw(InputKind::TouchUp));
  CHECK(h.probe.events.empty());
}

TEST_CASE("a broken knob module gives its ring back to the config color") {
  Harness h;
  Config c;
  c.knobs[1] = Harness::slot({{"ring", true}, {"throw_event", true}});
  h.deck.apply(c, h.lookup);
  REQUIRE(h.sink.ring_calls.back()[1] == Rgb{255, 0, 0});
  h.deck.on_input(raw(InputKind::KnobPress, 1));  // throws -> broken
  CHECK(h.sink.ring_calls.back()[1] == c.ring_colors[1]);
}

TEST_CASE("http responses reach a live module on the loop") {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  FakeHttp http;
  Probe probe;
  std::vector<TestModule*> created;
  ModuleInfo info;
  info.id = "test";
  info.slots = {SlotKind::Key};
  info.settings_schema = loose_schema();
  info.create = [&] { auto m = std::make_unique<TestModule>(&probe, 0); created.push_back(m.get()); return m; };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* { return id == "test" ? &info : nullptr; };
  Deck deck(loop, renderer, sink, default_device().layout, DeckServices{&http, nullptr});
  Config c;
  c.keys[0] = Harness::slot();
  deck.apply(c, lookup);
  created[0]->fetch();
  REQUIRE(http.pending.size() == 1);
  http.pending[0](HttpResponse{200, "ok", ""});
  loop.drain();
  REQUIRE(probe.xs.size() == 1);
  CHECK(probe.xs[0] == 200);
}

TEST_CASE("http responses for a destroyed module are dropped") {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  FakeHttp http;
  Probe probe;
  std::vector<TestModule*> created;
  ModuleInfo info;
  info.id = "test";
  info.slots = {SlotKind::Key};
  info.settings_schema = loose_schema();
  info.create = [&] { auto m = std::make_unique<TestModule>(&probe, 0); created.push_back(m.get()); return m; };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* { return id == "test" ? &info : nullptr; };
  Deck deck(loop, renderer, sink, default_device().layout, DeckServices{&http, nullptr});
  Config c;
  c.keys[0] = Harness::slot();
  deck.apply(c, lookup);
  created[0]->fetch();
  REQUIRE(http.wanted[0]);
  CHECK(http.wanted[0]());
  deck.apply(Config{}, lookup);  // module destroyed while the request is in flight
  CHECK_FALSE(http.wanted[0]());  // a request still waiting in the client is never sent
  http.pending[0](HttpResponse{200, "ok", ""});
  loop.drain();
  CHECK(probe.xs.empty());
}

TEST_CASE("a module's HttpRequest reaches the client whole; one isolated request in flight per slot") {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  FakeHttp http;
  Probe probe;
  std::vector<TestModule*> created;
  ModuleInfo info;
  info.id = "test";
  info.slots = {SlotKind::Key};
  info.settings_schema = loose_schema();
  info.create = [&] { auto m = std::make_unique<TestModule>(&probe, 0); created.push_back(m.get()); return m; };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* { return id == "test" ? &info : nullptr; };
  Deck deck(loop, renderer, sink, default_device().layout, DeckServices{&http, nullptr});
  Config c;
  c.keys[0] = Harness::slot();
  c.keys[1] = Harness::slot();
  deck.apply(c, lookup);
  REQUIRE(created.size() == 2);
  const HttpRequest req{.url = "http://ha.local/api?k=v",
                        .header = std::pair<std::string, std::string>{"Authorization", "Bearer t0k"},
                        .allow_plain_http = true,
                        .lane = HttpLane::Isolated};
  std::vector<std::string> got;
  auto record = [&](const HttpResponse& r) { got.push_back(r.error.empty() ? std::to_string(r.status) : r.error); };
  created[0]->ctx->http_get(req, record);
  REQUIRE(http.requests.size() == 1);
  CHECK(http.requests[0].url == req.url);
  REQUIRE(http.requests[0].header);
  CHECK(http.requests[0].header->first == "Authorization");
  CHECK(http.requests[0].header->second == "Bearer t0k");
  CHECK(http.requests[0].allow_plain_http);
  CHECK(http.requests[0].lane == HttpLane::Isolated);

  created[0]->ctx->http_get(req, record);  // same slot, first one still in flight: refused
  created[1]->ctx->http_get(req, record);  // another slot: sent
  created[0]->ctx->http_get("https://x", record);  // the shared lane is not limited
  loop.drain();
  CHECK(http.requests.size() == 3);
  REQUIRE(got.size() == 1);
  CHECK(got[0] == "busy");

  http.pending[0](HttpResponse{200, "{}", ""});
  loop.drain();
  REQUIRE(got.size() == 2);
  CHECK(got[1] == "200");
  created[0]->ctx->http_get(req, record);  // answered: the slot may ask again
  CHECK(http.requests.size() == 4);
}

TEST_CASE("without an http client, http_get answers with an error") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.created[0]->fetch();
  h.loop.drain();
  REQUIRE(h.probe.xs.size() == 1);
  CHECK(h.probe.xs[0] == 0);
}

TEST_CASE("strip zone limits: the first pixel of a zone belongs to it, 800 to none") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{430, Harness::slot()}, ZoneConfig{370, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  for (int x : {0, 429, 430, 799, 800}) {
    h.deck.on_input(raw(InputKind::TouchDown, 0, 0, x));
    h.deck.on_input(raw(InputKind::TouchUp));
  }
  REQUIRE(h.probe.events.size() == 4);
  CHECK(h.probe.events[0].first == 0);
  CHECK(h.probe.xs[0] == 0);
  CHECK(h.probe.events[1].first == 0);
  CHECK(h.probe.xs[1] == 429);
  CHECK(h.probe.events[2].first == 1);
  CHECK(h.probe.xs[2] == 0);
  CHECK(h.probe.events[3].first == 1);
  CHECK(h.probe.xs[3] == 369);
}

TEST_CASE("apply restarts only the slots whose config changed") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  c.keys[1] = Harness::slot();
  c.strip = {ZoneConfig{800, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 3);
  REQUIRE(h.sink.key_frames[0] == 1);
  c.keys[1] = Harness::slot({{"slow", true}});
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 4);          // only key 1 was rebuilt
  CHECK(h.sink.key_frames[0] == 1);      // untouched keys are not sent again
  CHECK(h.sink.key_frames[1] == 2);
  CHECK(h.sink.key_frames[5] == 1);      // nor are empty ones
  CHECK(h.sink.strip_frames == 1);       // the strip did not change
  CHECK(h.sink.ring_calls.size() == 1);  // nor did the rings
}

TEST_CASE("applying the same config twice does nothing") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  c.knobs[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);
  CHECK(h.sink.key_frames[0] == 1);
  CHECK(h.sink.strip_frames == 1);
  CHECK(h.sink.ring_calls.size() == 1);
}

TEST_CASE("a module moved to another key keeps its instance and its state; stale state is purged") {
  fs::path dir = fs::temp_directory_path() / "ld_deck_rekey";
  fs::remove_all(dir);
  Harness h;
  StateStore store(dir / "state.json");
  store.set("ghost:k4", 1);
  Deck deck{h.loop, h.renderer, h.sink, default_device().layout, DeckServices{nullptr, &store}};
  Config c;
  c.keys[0] = Harness::slot();
  deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(store.get("ghost:k4").is_null());
  h.created[0]->ctx->save_state({{"v", 7}});
  c.keys[0].reset();
  c.keys[3] = Harness::slot();
  deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 1);          // same instance, now on key 3
  CHECK(store.get("test:k3")["v"] == 7);
  CHECK(store.get("test:k0").is_null());
  CHECK(h.sink.key_frames[3] == 2);
  CHECK(h.sink.key_frames[0] == 2);      // key 0 is now empty: drawn black once more
  h.created[0]->ctx->save_state({{"v", 8}});
  CHECK(store.get("test:k3")["v"] == 8);  // later saves use the new key
  fs::remove_all(dir);
}

TEST_CASE("swapped zones keep their modules, their state and their gestures") {
  fs::path dir = fs::temp_directory_path() / "ld_deck_swap";
  fs::remove_all(dir);
  Harness h;
  StateStore store(dir / "state.json");
  Deck deck{h.loop, h.renderer, h.sink, default_device().layout, DeckServices{nullptr, &store}};
  Config c;
  c.strip = {ZoneConfig{400, Harness::slot()}, ZoneConfig{400, Harness::slot({{"slow", true}})}};
  deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  h.created[0]->ctx->save_state({{"who", 0}});
  h.created[1]->ctx->save_state({{"who", 1}});
  std::swap(c.strip[0], c.strip[1]);
  deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);
  CHECK(h.sink.strip_frames == 2);       // redrawn once, with the zones swapped
  CHECK(store.get("test:z0")["who"] == 1);
  CHECK(store.get("test:z1")["who"] == 0);
  deck.on_input(raw(InputKind::TouchDown, 0, 0, 100));
  deck.on_input(raw(InputKind::TouchUp));
  REQUIRE(h.probe.events.size() == 1);
  CHECK(h.probe.events[0].first == 1);   // the module that was on the right is now on the left
  fs::remove_all(dir);
}

TEST_CASE("a slot whose settings change is rebuilt but keeps its state key") {
  fs::path dir = fs::temp_directory_path() / "ld_deck_settings";
  fs::remove_all(dir);
  Harness h;
  StateStore store(dir / "state.json");
  Deck deck{h.loop, h.renderer, h.sink, default_device().layout, DeckServices{nullptr, &store}};
  Config c;
  c.keys[0] = Harness::slot();
  deck.apply(c, h.lookup);
  h.created[0]->ctx->save_state({{"v", 1}});
  c.keys[0] = Harness::slot({{"slow", true}});
  deck.apply(c, h.lookup);
  CHECK(h.created.size() == 2);
  CHECK(store.get("test:k0")["v"] == 1);
  fs::remove_all(dir);
}

TEST_CASE("a zone whose width changes is rebuilt") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{430, Harness::slot()}, ZoneConfig{370, Harness::slot({{"slow", true}})}};
  h.deck.apply(c, h.lookup);
  c.strip[0].width = 400;
  c.strip[1].width = 400;
  h.deck.apply(c, h.lookup);
  CHECK(h.created.size() == 4);
}

TEST_CASE("only a ring_brightness change re-sends the rings") {
  Harness h;
  Config c;
  h.deck.apply(c, h.lookup);
  REQUIRE(h.sink.ring_calls.size() == 1);
  c.ring_brightness = 30;
  h.deck.apply(c, h.lookup);
  REQUIRE(h.sink.ring_calls.size() == 2);
  CHECK(h.sink.ring_levels.back() == 30);
  c.ring_colors[2] = Rgb{1, 2, 3};
  h.deck.apply(c, h.lookup);
  REQUIRE(h.sink.ring_calls.size() == 3);
  CHECK(h.sink.ring_calls.back()[2] == Rgb{1, 2, 3});
}

namespace {
struct ReapplyModule : Module {
  std::function<void()> on_start;
  void start(ModuleContext&, const json&) override {
    if (on_start) on_start();
  }
  void render(Canvas&) override {}
};
}  // namespace

TEST_CASE("apply re-entered from a module start runs after the current one, not inside it") {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  Deck* deck_ptr = nullptr;
  int starts = 0;
  ModuleInfo info;
  info.id = "re";
  info.name = "re";
  info.slots = {SlotKind::Key};
  info.settings_schema = json::parse(R"({"fields":[]})");
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* { return id == "re" ? &info : nullptr; };
  Config second;
  second.keys[1] = SlotConfig{"re", json{{"x", 1}}};  // other settings: a new instance, not a move
  info.create = [&] {
    auto m = std::make_unique<ReapplyModule>();
    if (++starts == 1) m->on_start = [&] { deck_ptr->apply(second, lookup); };
    return m;
  };
  Deck deck(loop, renderer, sink, default_device().layout);
  deck_ptr = &deck;
  Config first;
  first.keys[0] = SlotConfig{"re", json::object()};
  deck.apply(first, lookup);
  CHECK(starts == 1);  // the nested apply did not run inside the first one
  loop.drain();
  CHECK(starts == 2);  // it ran afterwards: key 1 holds the module, key 0 is empty
}

namespace {
Rgb decode_pixel(Renderer& r, const std::vector<uint8_t>& jpeg, int x, int y) {
  Microsoft::WRL::ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(jpeg.data(), static_cast<UINT>(jpeg.size())));
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
  REQUIRE(SUCCEEDED(r.wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec)));
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  REQUIRE(SUCCEEDED(dec->GetFrame(0, &frame)));
  Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
  r.wic()->CreateFormatConverter(&conv);
  conv->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
  BYTE px[3];
  WICRect rc{x, y, 1, 1};
  conv->CopyPixels(&rc, 3, 3, px);
  return Rgb{px[2], px[1], px[0]};
}

bool close_rgb(Rgb a, Rgb b) {  // not "near": <windows.h> defines near as a macro
  return std::abs(a.r - b.r) <= 40 && std::abs(a.g - b.g) <= 40 && std::abs(a.b - b.b) <= 40;
}

// The strip's (-1) or a key's last frame as the web previews encode it (JPEG 90, unrotated); empty if none.
std::vector<uint8_t> preview_jpeg(Deck& deck, int target) {
  static JpegEncoder jpeg;
  std::vector<uint8_t> out;
  deck.preview(target, [&](const Image& i) { out = jpeg.encode(i, 90, Orientation::None); });
  return out;
}

struct LeftHalfModule : Module {  // left half red, right half black
  void start(ModuleContext&, const json&) override {}
  void render(Canvas& c) override { c.fill_rect(RectF{0, 0, c.width() / 2, c.height()}, Rgb{255, 0, 0}); }
};
}  // namespace

TEST_CASE("preview lends unrotated frames of keys and strip; on_frame names each new frame") {
  FakeLoop loop;
  Renderer renderer;
  RecordingSink sink;
  ModuleInfo info;
  info.id = "half";
  info.name = "half";
  info.slots = {SlotKind::Key, SlotKind::StripZone};
  info.settings_schema = json::parse(R"({"fields":[]})");
  info.create = [] { return std::make_unique<LeftHalfModule>(); };
  ModuleLookup lookup = [&](std::string_view id) -> const ModuleInfo* { return id == "half" ? &info : nullptr; };
  Deck deck(loop, renderer, sink, default_device().layout);
  std::vector<int> frames;
  deck.on_frame = [&](int target) { frames.push_back(target); };
  Config c;
  c.keys[2] = SlotConfig{"half", json::object()};
  c.strip = {ZoneConfig{800, SlotConfig{"half", json::object()}}};
  deck.apply(c, lookup);
  loop.drain();
  CHECK(frames.size() == 11);  // 10 keys + the strip
  CHECK(std::count(frames.begin(), frames.end(), -1) == 1);
  JpegEncoder jpeg;
  auto encoded = [&](int target) {
    std::vector<uint8_t> out;
    deck.preview(target, [&](const Image& i) { out = jpeg.encode(i, 90, Orientation::None); });
    return out;
  };
  auto key = encoded(2);
  REQUIRE(key.size() > 100);
  CHECK(close_rgb(decode_pixel(renderer, key, 10, 56), Rgb{255, 0, 0}));  // left stays on the left
  CHECK(close_rgb(decode_pixel(renderer, key, 100, 56), Rgb{0, 0, 0}));
  auto strip = encoded(-1);
  REQUIRE(strip.size() > 100);
  CHECK(close_rgb(decode_pixel(renderer, strip, 50, 56), Rgb{255, 0, 0}));
  CHECK(close_rgb(decode_pixel(renderer, strip, 750, 56), Rgb{0, 0, 0}));
  CHECK_FALSE(deck.preview(10, [](const Image&) {}));
  CHECK_FALSE(deck.preview(-2, [](const Image&) {}));
}

TEST_CASE("each slot renders with the global font unless it names its own; a missing font falls back to Segoe UI") {
  Harness h;
  Config c;
  c.appearance = Appearance{"Consolas", true};
  c.keys[0] = Harness::slot();
  SlotConfig own = Harness::slot();
  own.font = "Segoe UI Symbol";
  own.italic = false;
  c.keys[1] = own;
  SlotConfig gone = Harness::slot();
  gone.font = "No Such Font 4242";
  c.keys[2] = gone;
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 3);
  CHECK(h.probe.last.at(0).font == L"Consolas");
  CHECK(h.probe.last.at(0).italic);
  CHECK(h.probe.last.at(1).font == L"Segoe UI Symbol");
  CHECK_FALSE(h.probe.last.at(1).italic);
  CHECK(h.probe.last.at(2).font == L"Segoe UI");  // not installed: drawn in the default font
  CHECK(h.probe.last.at(2).italic);
  CHECK(h.probe.last.at(0).size == 20.f);          // no override: the element's default
}

TEST_CASE("a config without appearance renders every slot in upright Segoe UI") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.probe.last.at(0).font == L"Segoe UI");
  CHECK_FALSE(h.probe.last.at(0).italic);
}

TEST_CASE("a font or element size change redraws the slot without restarting its module") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  c.keys[1] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  REQUIRE(h.sink.key_frames[0] == 1);
  c.keys[0]->elements["label"] = ElementOverride{30, 600};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);       // same instances
  CHECK(h.sink.key_frames[0] == 2);   // key 0 redrawn
  CHECK(h.sink.key_frames[1] == 1);   // key 1 untouched
  CHECK(h.probe.last.at(0).size == 30.f);
  c.keys[0]->font = "Consolas";
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);
  CHECK(h.sink.key_frames[0] == 3);
  CHECK(h.probe.last.at(0).font == L"Consolas");
}

TEST_CASE("a global font change redraws the slots that inherit it, and only them") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  SlotConfig own = Harness::slot();
  own.font = "Segoe UI Symbol";
  c.keys[1] = own;
  c.strip = {ZoneConfig{800, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 3);
  c.appearance.font = "Consolas";
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 3);
  CHECK(h.sink.key_frames[0] == 2);
  CHECK(h.sink.key_frames[1] == 1);   // has its own font
  CHECK(h.sink.strip_frames == 2);
  CHECK(h.probe.last.at(0).font == L"Consolas");
  CHECK(h.probe.last.at(1).font == L"Segoe UI Symbol");
  CHECK(h.probe.last.at(2).font == L"Consolas");  // the zone
  c.appearance.italic = true;         // key 1 inherits the italic
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.sink.key_frames[1] == 2);
  CHECK(h.probe.last.at(1).italic);
}

TEST_CASE("a module moved to another key keeps its font and element sizes") {
  Harness h;
  Config c;
  SlotConfig styled = Harness::slot();
  styled.font = "Consolas";
  styled.elements["label"] = ElementOverride{44, std::nullopt};
  c.keys[0] = styled;
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  c.keys[0].reset();
  c.keys[3] = styled;
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 1);      // the same instance, now on key 3
  CHECK(h.sink.key_frames[3] == 2);
  CHECK(h.probe.last.at(0).font == L"Consolas");
  CHECK(h.probe.last.at(0).size == 44.f);
}

TEST_CASE("two identical modules that swap fonts are redrawn with their new fonts") {
  Harness h;
  Config c;
  SlotConfig a = Harness::slot(), b = Harness::slot();
  a.font = "Consolas";
  b.font = "Segoe UI Symbol";
  c.strip = {ZoneConfig{400, a}, ZoneConfig{400, b}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  std::swap(c.strip[0].slot->font, c.strip[1].slot->font);
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);        // same settings: each instance stays in place
  CHECK(h.sink.strip_frames == 2);
  CHECK(h.probe.last.at(0).font == L"Segoe UI Symbol");
  CHECK(h.probe.last.at(1).font == L"Consolas");
}

TEST_CASE("a missing font is logged once, however many slots or renders use it") {
  fs::path log = fs::temp_directory_path() / "lite_deck_test_missing_font.log";
  std::error_code ec;
  fs::remove(log, ec);
  log_init(log, LogLevel::Warn);
  struct RestoreLog {  // leave the global log as other tests expect it, even if this one throws
    ~RestoreLog() { log_init({}, LogLevel::Info); }
  } restore;
  {
    Harness h;
    Config c;
    c.appearance.font = "No Such Font 4242";
    c.keys[0] = Harness::slot();
    c.keys[1] = Harness::slot();
    h.deck.apply(c, h.lookup);
    h.loop.drain();
    c.keys[0]->elements["label"] = ElementOverride{30, std::nullopt};  // another render, same missing font
    h.deck.apply(c, h.lookup);
    h.loop.drain();
    h.deck.redraw_all();
    h.loop.drain();
  }
  log_init({}, LogLevel::Info);  // closes the file
  std::ifstream in(log);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string needle = "font \"No Such Font 4242\" is not installed: Segoe UI is used instead";
  std::size_t count = 0;
  for (std::size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++count;
  CHECK(count == 1);
  in.close();
  fs::remove(log, ec);
}

TEST_CASE("a role change redraws the keys without restarting their modules; the strip keeps its look") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  c.strip = {ZoneConfig{800, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);  // id 0: the key, id 1: the zone
  REQUIRE(h.sink.key_frames[0] == 1);
  REQUIRE(h.sink.strip_frames == 1);
  CHECK(h.probe.last.at(0).value_size == 36.f);
  c.appearance.roles[TextRole::Value] = RoleOverride{50, 400, "Consolas"};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);      // no module restarted
  CHECK(h.sink.key_frames[0] == 2);  // the key redrawn
  CHECK(h.sink.strip_frames == 1);   // the strip has no roles: not redrawn
  CHECK(h.probe.last.at(0).value_size == 50.f);
  CHECK(h.probe.last.at(0).value_font == L"Consolas");
  CHECK(h.probe.last.at(0).size == 20.f);         // the probe label has no role
  CHECK(h.probe.last.at(1).value_size == 36.f);   // the zone
  CHECK(h.probe.last.at(1).value_font.empty());
  c.appearance.roles.clear();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);
  CHECK(h.sink.key_frames[0] == 3);
  CHECK(h.probe.last.at(0).value_size == 36.f);
  CHECK(h.probe.last.at(0).value_font.empty());
}

TEST_CASE("a role font yields to the key's own font, and a missing one falls back to the font the key would use") {
  Harness h;
  Config c;
  c.appearance.font = "Segoe UI Symbol";
  c.appearance.roles[TextRole::Value].font = "Consolas";
  c.keys[0] = Harness::slot();  // inherits the global font: the role's font applies
  SlotConfig own = Harness::slot();
  own.font = "Segoe UI";  // its own font wins over the role's
  c.keys[1] = own;
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  CHECK(h.probe.last.at(0).value_font == L"Consolas");
  CHECK(h.probe.last.at(0).font == L"Segoe UI Symbol");  // the other texts keep the global font
  CHECK(h.probe.last.at(1).value_font.empty());          // "" = the key's own font
  CHECK(h.probe.last.at(1).font == L"Segoe UI");
  c.appearance.roles[TextRole::Value].font = "No Such Font 4242";
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);
  CHECK(h.probe.last.at(0).value_font.empty());          // missing: the global font, not Segoe UI
  CHECK(h.probe.last.at(0).font == L"Segoe UI Symbol");
}

TEST_CASE("a missing role font is logged once, naming the fallback") {
  fs::path log = fs::temp_directory_path() / "lite_deck_test_missing_role_font.log";
  std::error_code ec;
  fs::remove(log, ec);
  log_init(log, LogLevel::Warn);
  struct RestoreLog {  // leave the global log as other tests expect it, even if this one throws
    ~RestoreLog() { log_init({}, LogLevel::Info); }
  } restore;
  {
    Harness h;
    Config c;
    c.appearance.roles[TextRole::Label].font = "No Such Role Font 4242";
    c.keys[0] = Harness::slot();
    c.keys[1] = Harness::slot();
    h.deck.apply(c, h.lookup);
    h.loop.drain();
    c.appearance.roles[TextRole::Label].size = 30;  // another look, same missing font
    h.deck.apply(c, h.lookup);
    h.loop.drain();
    h.deck.redraw_all();
    h.loop.drain();
  }
  log_init({}, LogLevel::Info);  // closes the file
  std::ifstream in(log);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string needle = "font \"No Such Role Font 4242\" is not installed: the key's font is used instead";
  std::size_t count = 0;
  for (std::size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++count;
  CHECK(count == 1);
  in.close();
  fs::remove(log, ec);
}

TEST_CASE("an animating zone re-renders alone; the untouched zone keeps its pixels and the whole strip is sent") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{300, Harness::slot({{"anim", true}, {"text", true}})}, ZoneConfig{250, Harness::slot()},
             ZoneConfig{250, Harness::slot({{"text", true}})}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 3);
  CHECK(h.probe.renders_of[0] == 1);
  CHECK(h.probe.renders_of[1] == 1);
  CHECK(h.probe.renders_of[2] == 1);
  const int before = h.sink.strip_frames;
  h.loop.advance(1000ms);
  CHECK(h.sink.strip_frames - before >= 28);     // a full strip image per frame
  CHECK(h.probe.renders_of[0] - 1 == h.sink.strip_frames - before);
  CHECK(h.probe.renders_of[1] == 1);             // never re-rendered
  CHECK(h.probe.renders_of[2] == 1);
  h.created[0]->ctx->invalidate();  // one more partial frame, at the current instant
  h.loop.drain();
  CHECK(h.probe.renders_of[1] == 1);
  auto strip = preview_jpeg(h.deck, -1);
  CHECK(close_rgb(decode_pixel(h.renderer, strip, 420, 56), Rgb{0, 200, 0}));  // zone 1 kept its frame
  // What the partial frames built is exactly what a full redraw at the same instant draws.
  h.deck.redraw_all();
  h.loop.drain();
  CHECK(h.probe.renders_of[1] == 2);
  CHECK(h.probe.renders_of[2] == 2);
  CHECK(preview_jpeg(h.deck, -1) == strip);
}

TEST_CASE("a re-rendered zone starts from a cleared rect") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{400, Harness::slot({{"once", true}})}, ZoneConfig{400, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  CHECK(close_rgb(decode_pixel(h.renderer, preview_jpeg(h.deck, -1), 200, 56), Rgb{0, 200, 0}));
  h.created[0]->ctx->invalidate();  // draws nothing this time
  h.loop.drain();
  auto strip = preview_jpeg(h.deck, -1);
  CHECK(close_rgb(decode_pixel(h.renderer, strip, 200, 56), Rgb{0, 0, 0}));
  CHECK(close_rgb(decode_pixel(h.renderer, strip, 600, 56), Rgb{0, 200, 0}));
  CHECK(h.probe.renders_of[1] == 1);
}

TEST_CASE("a zone layout change, a redraw or a resume renders every zone") {
  Harness h;
  Config c;
  c.strip = {ZoneConfig{400, Harness::slot()}, ZoneConfig{400, Harness::slot({{"slow", true}})}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  std::swap(c.strip[0], c.strip[1]);  // both modules kept, both moved
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  CHECK(h.probe.renders_of[0] == 2);
  CHECK(h.probe.renders_of[1] == 2);
  h.deck.redraw_all();
  h.loop.drain();
  CHECK(h.probe.renders_of[0] == 3);
  CHECK(h.probe.renders_of[1] == 3);
  h.deck.suspend();
  h.deck.resume();
  h.loop.drain();
  CHECK(h.probe.renders == 6 + 2);  // new instances after the resume, both drawn
}

TEST_CASE("a palette change redraws every slot with the new colours without restarting its module") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  c.strip = {ZoneConfig{800, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 2);
  REQUIRE(h.sink.key_frames[0] == 1);
  CHECK(h.probe.last.at(0).stale == StatePalette{}.stale);
  c.appearance.states.stale = Rgb{1, 2, 3};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 2);      // same instances
  CHECK(h.sink.key_frames[0] == 2);  // redrawn
  CHECK(h.probe.last.at(0).stale == Rgb{1, 2, 3});
  CHECK(h.probe.last.at(1).stale == Rgb{1, 2, 3});
}

TEST_CASE("an element-only change redraws only that slot") {
  Harness h;
  Config c;
  c.keys[0] = Harness::slot();
  c.keys[1] = Harness::slot();
  c.keys[2] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 3);
  c.keys[1]->elements["label"] = ElementOverride{.visible = false, .align = HAlign::Right};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.created.size() == 3);
  CHECK(h.sink.key_frames[0] == 1);
  CHECK(h.sink.key_frames[1] == 2);
  CHECK(h.sink.key_frames[2] == 1);
}

TEST_CASE("a knob that sets a black ring keeps it black; the Device colour is for empty knobs") {
  Harness h;
  Config c;
  c.knobs[2] = Harness::slot({{"ring_black", true}});
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE_FALSE(h.sink.ring_calls.empty());
  CHECK(h.sink.ring_calls.back()[2] == Rgb{0, 0, 0});
  CHECK(h.sink.ring_calls.back()[0] == c.ring_colors[0]);  // empty knob
}

TEST_CASE("a knob release reaches the knob's module as Release; an unknown knob is ignored") {
  Harness h;
  Config c;
  c.knobs[1] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  h.deck.on_input(raw(InputKind::KnobPress, 1));
  h.deck.on_input(raw(InputKind::KnobRelease, 1));
  h.deck.on_input(raw(InputKind::KnobRelease, 7));
  REQUIRE(h.probe.events.size() == 2);
  CHECK(h.probe.events[0].second == EventKind::Press);
  CHECK(h.probe.events[1].second == EventKind::Release);
}

TEST_CASE("on the Test Deck: 15 keys of 96 x 96 px, no strip, two knobs, no ring call, input in the grid only") {
  Harness h(fake::test_deck_layout());
  Config c;
  c.keys.resize(15);
  c.keys[12] = Harness::slot();
  c.knobs[1] = Harness::slot();
  c.strip = {ZoneConfig{800, Harness::slot()}};  // kept in the config, never started
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.sink.key_frames.size() == 15);
  CHECK(std::all_of(h.sink.key_sizes.begin(), h.sink.key_sizes.end(),
                    [](const std::pair<int, int>& s) { return s == std::pair{96, 96}; }));
  CHECK(h.sink.strip_frames == 0);
  CHECK(h.sink.ring_calls.empty());
  CHECK(h.created.size() == 2);  // key 12 (id 0) and knob 1 (id 1)
  h.deck.on_input(raw(InputKind::KeyDown, 12));
  h.deck.on_input(raw(InputKind::KeyUp, 12));
  h.deck.on_input(raw(InputKind::KnobTurn, 1, +1));
  h.deck.on_input(raw(InputKind::TouchDown, 0, 0, 100));  // no strip: nobody
  h.deck.on_input(raw(InputKind::TouchUp));
  h.deck.on_input(raw(InputKind::KeyDown, 15));           // beyond the grid: ignored
  h.deck.on_input(raw(InputKind::KnobPress, 2));          // no third knob: ignored
  REQUIRE(h.probe.events.size() == 4);
  CHECK(h.probe.events[0] == std::pair{0, EventKind::Press});
  CHECK(h.probe.events[1] == std::pair{0, EventKind::Release});
  CHECK(h.probe.events[2] == std::pair{0, EventKind::Tap});
  CHECK(h.probe.events[3] == std::pair{1, EventKind::Rotate});
  CHECK_FALSE(h.deck.preview(-1, [](const Image&) {}));  // no strip to preview
  int width = 0;
  CHECK(h.deck.preview(14, [&](const Image& i) { width = i.width; }));
  CHECK(width == 96);
}

TEST_CASE("a long press works on the last key of a 15-key device") {
  Harness h(fake::test_deck_layout());
  Config c;
  c.keys.resize(15);
  c.keys[14] = Harness::slot();
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  h.deck.on_input(raw(InputKind::KeyDown, 14));
  h.loop.advance(600ms);
  h.deck.on_input(raw(InputKind::KeyUp, 14));
  REQUIRE(h.probe.events.size() == 3);
  CHECK(h.probe.events[0].second == EventKind::Press);
  CHECK(h.probe.events[1].second == EventKind::LongPress);
  CHECK(h.probe.events[2].second == EventKind::Release);
}

TEST_CASE("a device with six ringed knobs gets six colours: the four defaults, then dark grey") {
  DeviceLayout six = default_device().layout;
  six.id = "six-rings";
  six.knobs = 6;
  Harness h(six);
  h.deck.apply(Config{}, h.lookup);
  h.loop.drain();
  REQUIRE(h.sink.ring_calls.size() == 1);
  REQUIRE(h.sink.ring_calls[0].size() == 6);
  CHECK(h.sink.ring_calls[0][3] == Rgb{0x3d, 0x1f, 0x00});
  CHECK(h.sink.ring_calls[0][4] == Rgb{0x20, 0x20, 0x20});
  CHECK(h.sink.ring_calls[0][5] == Rgb{0x20, 0x20, 0x20});
}

TEST_CASE("a device with a 600 x 100 strip gets strip frames of that size") {
  DeviceLayout strip600 = default_device().layout;
  strip600.id = "strip600";
  strip600.strip = PixelSize{600, 100};
  Harness h(strip600);
  Config c;
  c.strip = {ZoneConfig{300, Harness::slot()}, ZoneConfig{300, Harness::slot()}};
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  CHECK(h.sink.strip_frames == 1);
  CHECK(h.sink.last_strip == std::pair{600, 100});
  CHECK(h.created.size() == 2);
}

TEST_CASE("on the N4 Pro, every key, knob and strip input reaches the module of its own slot") {
  Harness h;
  Config c;
  for (auto& k : c.keys) k = Harness::slot();   // ids 0-9, in key order
  for (auto& k : c.knobs) k = Harness::slot();  // ids 10-13
  c.strip = {ZoneConfig{430, Harness::slot()}, ZoneConfig{370, Harness::slot()}};  // ids 14, 15
  h.deck.apply(c, h.lookup);
  h.loop.drain();
  REQUIRE(h.created.size() == 16);
  using Events = std::vector<std::pair<int, EventKind>>;
  for (int i = 0; i < 10; ++i) {
    INFO("key " << i);
    h.probe.events.clear();
    h.deck.on_input(raw(InputKind::KeyDown, i));
    h.deck.on_input(raw(InputKind::KeyUp, i));
    CHECK(h.probe.events == Events{{i, EventKind::Press}, {i, EventKind::Release}, {i, EventKind::Tap}});
  }
  for (int i = 0; i < 4; ++i) {
    INFO("knob " << i);
    h.probe.events.clear();
    h.probe.deltas.clear();
    h.deck.on_input(raw(InputKind::KnobTurn, i, -1));
    h.deck.on_input(raw(InputKind::KnobTurn, i, +1));
    h.deck.on_input(raw(InputKind::KnobPress, i));
    h.deck.on_input(raw(InputKind::KnobRelease, i));
    CHECK(h.probe.events == Events{{10 + i, EventKind::Rotate}, {10 + i, EventKind::Rotate},
                                   {10 + i, EventKind::Press}, {10 + i, EventKind::Release}});
    CHECK(h.probe.deltas == std::vector<int>{-1, 1, 0, 0});
  }
  for (const auto& [x, id, rel] : {std::tuple{0, 14, 0}, std::tuple{429, 14, 429}, std::tuple{430, 15, 0},
                                   std::tuple{799, 15, 369}}) {
    INFO("touch at " << x);
    h.probe.events.clear();
    h.probe.xs.clear();
    h.deck.on_input(raw(InputKind::TouchDown, 0, 0, x));
    h.deck.on_input(raw(InputKind::TouchUp, 0, 0, x));
    REQUIRE(h.probe.events.size() == 1);
    CHECK(h.probe.events[0] == std::pair{id, EventKind::Tap});
    CHECK(h.probe.xs[0] == rel);
  }
}
