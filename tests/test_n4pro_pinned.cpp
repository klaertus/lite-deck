// tests/test_n4pro_pinned.cpp: the HID traffic of the N4 Pro, pinned before the device layer is
// generalised (device profiles plan, Task 1). Every report, its order and its time are compared with
// tests/n4pro_reference.*. Later tasks may only change the adapters marked "API:", never an expectation.
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <format>
#include <string>
#include <tuple>
#include <vector>
#include "device/device_manager.h"
#include "device/registry.h"
#include "fake_loop.h"
#include "layout/deck.h"
#include "n4pro_reference.h"
#include "test_util.h"

using namespace ld;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using nlohmann::json;
using ref::Bytes;

namespace {

long long ms_of(const FakeLoop& l) { return std::chrono::duration_cast<Duration>(l.now().time_since_epoch()).count(); }

struct Sent {
  long long ms;
  Bytes report;
  bool operator==(const Sent&) const = default;
};

// API: the link the DeviceManager opens; records every report with the loop time. Failure injector:
// the write that would become sent[fail_at] fails, and so does every later write on this link
// (each attempt is still recorded, so a driver that keeps writing to a dead link shows up).
// An empty `reads` makes finish_read report a read error.
struct PinLink final : DeviceLink {
  FakeLoop* loop;
  std::vector<Sent>* sent;
  std::deque<std::vector<std::uint8_t>> reads;
  std::size_t fail_at = SIZE_MAX;
  bool dead = false;
  PinLink(FakeLoop* l, std::vector<Sent>* s) : loop(l), sent(s) {}
  bool write(std::span<const std::uint8_t> r) override {
    if (sent->size() == fail_at) dead = true;
    sent->push_back({ms_of(*loop), Bytes(r.begin(), r.end())});
    return !dead;
  }
  void* read_event() override { return this; }
  bool start_read() override { return true; }
  std::optional<std::vector<std::uint8_t>> finish_read() override {
    if (reads.empty()) return std::nullopt;
    auto r = reads.front();
    reads.pop_front();
    return r;
  }
};

// What every pinned slot shows: its colour with a white block in the top-left corner (a rotation
// moves it), drawn with pixel-aligned rectangles only.
void draw_pin(Canvas& c, Rgb color) {
  c.clear(color);
  c.fill_rect(RectF{0, 0, c.width() / 4, c.height() / 2}, Rgb{255, 255, 255});
}

struct PinModule final : Module {
  Rgb color;
  void start(ModuleContext&, const json& s) override { color = parse_color(s.at("color").get<std::string>()).value(); }
  void render(Canvas& c) override { draw_pin(c, color); }
};

const ModuleInfo& pin_info() {
  static const ModuleInfo info = [] {
    ModuleInfo m;
    m.id = "pin";
    m.name = "Pin";
    m.slots = {SlotKind::Key, SlotKind::StripZone};
    m.settings_schema = json::parse(R"({"fields":[{"key":"color","type":"color","default":"#000000","label":"Color"}]})");
    m.create = [] { return std::make_unique<PinModule>(); };
    return m;
  }();
  return info;
}
const ModuleInfo* pin_lookup(std::string_view id) { return id == "pin" ? &pin_info() : nullptr; }

Rgb key_color(int i) {
  return Rgb{static_cast<std::uint8_t>(20 * i), static_cast<std::uint8_t>(200 - 15 * i), 90};
}
constexpr Rgb kStripColor{30, 60, 200};

Config pin_config() {
  Config c;
  for (int i = 0; i < 10; ++i) c.keys[i] = SlotConfig{"pin", json{{"color", format_color(key_color(i))}}};
  c.strip = {ZoneConfig{800, SlotConfig{"pin", json{{"color", format_color(kStripColor)}}}}};
  c.ring_brightness = 80;
  return c;
}

Bytes expected_jpeg(Renderer& r, int w, int h, Rgb color, int quality) {
  Surface s(r, w, h);
  {
    Canvas c = s.canvas();
    c.clear(Rgb{0, 0, 0});
    draw_pin(c, color);
  }
  return ref::jpeg(r, s, quality, true);
}

struct Expect {
  std::vector<Sent> out;
  void at(long long ms, Bytes r) { out.push_back({ms, std::move(r)}); }
  void at(long long ms, const std::vector<Bytes>& rs) {
    for (const Bytes& r : rs) out.push_back({ms, r});
  }
};

void expect_init(Expect& e, long long ms, std::uint8_t brightness) {
  e.at(ms, ref::command("DIS"));
  e.at(ms, ref::command("LIG\0\0"sv, {brightness}));
  e.at(ms, ref::command("CLE\0\0\0"sv, {0xFF}));
  e.at(ms, ref::command("BGCLE", {3}));
  e.at(ms, ref::command("STP"));
}

// The four default ring colours (#3e0000 #002132 #1b3500 #3d1f00) at `level`.
void expect_rings(Expect& e, long long ms, std::uint8_t level) {
  e.at(ms, ref::command("LBLIG", {level}));
  e.at(ms, ref::command("SETLB", {0x3e, 0x00, 0x00, 0x00, 0x21, 0x32, 0x1b, 0x35, 0x00, 0x3d, 0x1f, 0x00}));
}

void expect_frames(Expect& e, long long ms, const std::vector<Bytes>& keys, const Bytes& strip) {
  e.at(ms, ref::image_bg(strip, 0, 0, 800, 112));
  for (int i = 0; i < 10; ++i) e.at(ms, ref::image_key(ref::key_index(i), keys[static_cast<std::size_t>(i)]));
  e.at(ms, ref::command("STP"));
}

// API: one decoded input as text: "<kind> <index> <delta> <x> <y>".
std::string describe(const DeviceInput& in) {
  const char* name = "None";
  switch (in.kind) {
    case InputKind::KeyDown: name = "KeyDown"; break;
    case InputKind::KeyUp: name = "KeyUp"; break;
    case InputKind::KnobTurn: name = "KnobTurn"; break;
    case InputKind::KnobPress: name = "KnobPress"; break;
    case InputKind::KnobRelease: name = "KnobRelease"; break;
    case InputKind::TouchDown: name = "TouchDown"; break;
    case InputKind::TouchMove: name = "TouchMove"; break;
    case InputKind::TouchUp: name = "TouchUp"; break;
    case InputKind::None: break;
  }
  return std::format("{} {} {} {} {}", name, in.index, in.delta, in.x, in.y);
}

}  // namespace

TEST_CASE("pinned: the reference JPEG pipeline is the one Surface::encode_jpeg runs") {
  Renderer r;
  for (const auto& [w, h, q, rot] : {std::tuple{112, 112, 85, true}, std::tuple{800, 112, 80, true},
                                     std::tuple{112, 112, 90, false}, std::tuple{800, 112, 90, false}}) {
    INFO(w << "x" << h << " q" << q << (rot ? " rotated" : ""));
    Surface s(r, w, h);
    {
      Canvas c = s.canvas();
      c.clear(Rgb{0, 0, 0});
      draw_pin(c, Rgb{200, 40, 10});
    }
    CHECK(s.encode_jpeg(q, rot) == ref::jpeg(r, s, q, rot));
  }
}

TEST_CASE("pinned: the N4 Pro session sends exactly these reports, in this order, at these times") {
  FakeLoop loop;
  Renderer renderer;
  std::vector<Sent> sent;
  // API: how the DeviceManager and the Deck are built.
  DeviceManager dm(loop, default_device(),
                   [&]() -> std::unique_ptr<DeviceLink> { return std::make_unique<PinLink>(&loop, &sent); }, nullptr);
  Deck deck(loop, renderer, dm, default_device().layout);
  dm.on_connected = [&] { deck.redraw_all(); };
  dm.set_brightness(70);
  deck.apply(pin_config(), pin_lookup);
  loop.drain();
  CHECK(sent.empty());  // nothing reaches a device that is not connected yet

  dm.start();             // t = 0: init, the cached rings, the rings again from the redraw
  loop.drain();           // the redraw's frames are queued...
  loop.advance(1ms);      // ...and flushed at t = 0
  loop.advance(10000ms);  // heartbeat at t = 10000
  dm.set_brightness(40);  // t = 10001
  deck.suspend();         // what main.cpp does on lock
  dm.sleep();
  loop.advance(1000ms);
  deck.resume();          // t = 11001: what main.cpp does on unlock
  dm.wake();
  loop.drain();
  loop.advance(1ms);      // frames flushed at t = 11001
  dm.shutdown();          // t = 11002

  std::vector<Bytes> keys;
  for (int i = 0; i < 10; ++i) keys.push_back(expected_jpeg(renderer, 112, 112, key_color(i), 85));
  const Bytes strip = expected_jpeg(renderer, 800, 112, kStripColor, 80);
  Expect e;
  expect_init(e, 0, 70);
  expect_rings(e, 0, 80);
  expect_rings(e, 0, 80);
  expect_frames(e, 0, keys, strip);
  e.at(10000, ref::command("CONNECT"));
  e.at(10001, ref::command("LIG\0\0"sv, {40}));
  e.at(10001, ref::command("HAN"));
  e.at(10001, ref::command("LBLIG", {0}));
  expect_init(e, 11001, 40);
  expect_rings(e, 11001, 80);
  expect_rings(e, 11001, 80);
  expect_frames(e, 11001, keys, strip);
  e.at(11002, ref::command("CLE\0\0DC"sv));
  e.at(11002, ref::command("HAN"));

  REQUIRE(sent.size() == e.out.size());
  for (std::size_t i = 0; i < sent.size(); ++i) {
    INFO("report " << i << ": " << report_tag(sent[i].report) << " at " << sent[i].ms << " ms, expected "
                   << report_tag(e.out[i].report) << " at " << e.out[i].ms << " ms");
    CHECK(sent[i] == e.out[i]);
  }
}

TEST_CASE("pinned: N4 Pro input reports decode to these events; write acks and unknown codes reach nobody") {
  FakeLoop loop;
  std::vector<Sent> sent;
  PinLink* link = nullptr;
  // API: how the DeviceManager is built and what it hands out.
  DeviceManager dm(loop, default_device(), [&]() -> std::unique_ptr<DeviceLink> {
    auto l = std::make_unique<PinLink>(&loop, &sent);
    link = l.get();
    return l;
  }, nullptr);
  std::vector<std::string> got;
  dm.on_input = [&](const DeviceInput& in) { got.push_back(describe(in)); };
  dm.start();
  REQUIRE(link);
  const std::vector<std::pair<std::string, std::string>> table = {
      {"00 41 43 4b 00 00 4f 4b 00 00 01 01", "KeyDown 0 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 01 00", "KeyUp 0 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 02 01", "KeyDown 1 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 02 00", "KeyUp 1 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 03 01", "KeyDown 2 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 03 00", "KeyUp 2 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 04 01", "KeyDown 3 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 04 00", "KeyUp 3 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 05 01", "KeyDown 4 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 05 00", "KeyUp 4 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 06 01", "KeyDown 5 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 06 00", "KeyUp 5 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 07 01", "KeyDown 6 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 07 00", "KeyUp 6 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 08 01", "KeyDown 7 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 08 00", "KeyUp 7 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 09 01", "KeyDown 8 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 09 00", "KeyUp 8 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 0a 01", "KeyDown 9 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 0a 00", "KeyUp 9 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 a0 00", "KnobTurn 0 -1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 a1 00", "KnobTurn 0 1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 50 00", "KnobTurn 1 -1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 51 00", "KnobTurn 1 1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 90 00", "KnobTurn 2 -1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 91 00", "KnobTurn 2 1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 70 00", "KnobTurn 3 -1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 71 00", "KnobTurn 3 1 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 37 00", "KnobPress 0 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 35 00", "KnobPress 1 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 33 00", "KnobPress 2 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 36 00", "KnobPress 3 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 00 01 01 90", "TouchDown 0 0 400 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 00 00 01 90", ""},
      {"00 41 43 4b 00 41 52 58 00 00 00 01 2c 00 32", "TouchMove 0 0 300 50"},
      {"00 41 43 4b 00 00 4f 4b 00 00 40 00 02 58", "TouchUp 0 0 600 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 41 00 00 64", "TouchUp 0 0 100 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 42 00 01 2c", "TouchUp 0 0 300 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 43 00 03 1f", "TouchUp 0 0 799 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 38 00", "TouchUp 0 0 0 0"},
      {"00 41 43 4b 00 00 4f 4b 00 00 39 00 01 90", "TouchUp 0 0 0 0"},
      {"00 41 43 4c 00 00 4f 4b 00 00 01 01", ""},
      {"00 41 43 4b 00 00 4f 4c 00 00 01 01", ""},
      {"00 41 43 4b 00 00 4f 4b 00 00 ff 00", ""},
      {"00 41 43 4b 00 00 4f 4b 00 00 0b 01", ""},
  };
  for (const auto& [hex, want] : table) {
    INFO(hex);
    got.clear();
    link->reads.push_back(report_from_hex(hex));
    loop.signal(link);
    CHECK(got == (want.empty() ? std::vector<std::string>{} : std::vector<std::string>{want}));
  }
  // A short report (shorter than the 15 bytes a touch move needs) reaches nobody, and the link stays up.
  got.clear();
  link->reads.push_back({0x00, 0x41, 0x43, 0x4b, 0x00, 0x00, 0x4f, 0x4b, 0x00, 0x00, 0x01, 0x01});
  loop.signal(link);
  CHECK(got.empty());
  link->reads.push_back(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 02 01"));
  loop.signal(link);
  CHECK(got == std::vector<std::string>{"KeyDown 1 0 0 0"});
}

namespace {

// API: a session with a DeviceManager that opens a fresh PinLink on every connect (`link` = the
// current one) and a Deck redrawn on every (re)connection, as main.cpp wires them.
struct Session {
  FakeLoop loop;
  Renderer renderer;
  std::vector<Sent> sent;
  PinLink* link = nullptr;
  DeviceManager dm;
  Deck deck;
  Session()
      : dm(loop, default_device(),
           [this]() -> std::unique_ptr<DeviceLink> {
             auto l = std::make_unique<PinLink>(&loop, &sent);
             link = l.get();
             return l;
           },
           nullptr),
        deck(loop, renderer, dm, default_device().layout) {
    dm.on_connected = [this] { deck.redraw_all(); };
  }
};

void set_key(Config& c, int i, Rgb color) { c.keys[i] = SlotConfig{"pin", json{{"color", format_color(color)}}}; }
void set_strip(Config& c, Rgb color) { c.strip = {ZoneConfig{800, SlotConfig{"pin", json{{"color", format_color(color)}}}}}; }

// What the deck shows: one colour per key and the strip colour; JPEGs from the reference.
struct Look {
  Renderer& r;
  std::vector<Rgb> keys;
  Rgb strip;
  Bytes key(int i) const { return expected_jpeg(r, 112, 112, keys[static_cast<std::size_t>(i)], 85); }
  Bytes strip_jpeg() const { return expected_jpeg(r, 800, 112, strip, 80); }
  std::vector<Bytes> all_keys() const {
    std::vector<Bytes> out;
    for (int i = 0; i < 10; ++i) out.push_back(key(i));
    return out;
  }
};

// Reconnection after a lost link: init at the current brightness, the cached rings, the rings again
// from the redraw, then every frame in one flush.
void expect_reconnect(Expect& e, long long ms, std::uint8_t brightness, const Look& look) {
  expect_init(e, ms, brightness);
  expect_rings(e, ms, 80);
  expect_rings(e, ms, 80);
  expect_frames(e, ms, look.all_keys(), look.strip_jpeg());
}

void compare(const std::vector<Sent>& sent, const Expect& e) {
  CHECK(sent.size() == e.out.size());
  for (std::size_t i = 0; i < std::min(sent.size(), e.out.size()); ++i) {
    INFO("report " << i << ": " << report_tag(sent[i].report) << " at " << sent[i].ms << " ms, expected "
                   << report_tag(e.out[i].report) << " at " << e.out[i].ms << " ms");
    CHECK(sent[i] == e.out[i]);
  }
}

}  // namespace

TEST_CASE("pinned: N4 Pro pacing, ring and sleep edge cases, write failure and read error with reconnection") {
  Session s;
  Config cfg = pin_config();
  const auto default_rings = cfg.ring_colors;
  Look look{s.renderer, {}, kStripColor};
  for (int i = 0; i < 10; ++i) look.keys.push_back(key_color(i));
  auto apply = [&] {
    s.deck.apply(cfg, pin_lookup);
    s.loop.drain();
  };
  auto key = [&](int i, Rgb c) {
    set_key(cfg, i, c);
    look.keys[static_cast<std::size_t>(i)] = c;
  };
  auto strip = [&](Rgb c) {
    set_strip(cfg, c);
    look.strip = c;
  };
  Expect e;

  // t = 0: connect and first frames (as in the session above).
  s.dm.set_brightness(70);
  apply();
  s.dm.start();
  s.loop.drain();
  s.loop.advance(1ms);  // t = 1
  expect_init(e, 0, 70);
  expect_rings(e, 0, 80);
  expect_rings(e, 0, 80);
  expect_frames(e, 0, look.all_keys(), look.strip_jpeg());

  // Burst 1: both channels last sent at 0, so nothing goes out before 34 ms. Key 3 changes twice
  // (only its latest image is sent), key 0 once, the strip once: one flush at 34, strip first,
  // keys by slot, one STP.
  key(3, Rgb{250, 0, 0});
  apply();
  s.loop.advance(9ms);  // t = 10
  key(0, Rgb{0, 250, 0});
  key(3, Rgb{0, 0, 250});
  apply();
  s.loop.advance(10ms);  // t = 20
  strip(Rgb{200, 30, 30});
  apply();
  s.loop.advance(14ms);  // t = 34
  e.at(34, ref::image_bg(look.strip_jpeg(), 0, 0, 800, 112));
  e.at(34, ref::image_key(ref::key_index(0), look.key(0)));
  e.at(34, ref::image_key(ref::key_index(3), look.key(3)));
  e.at(34, ref::command("STP"));

  // Burst 2: the channels are paced independently. A strip at 50 waits for 68; a key at 80 goes
  // out at once (keys last sent at 34) while the strip queued with it waits for 102.
  s.loop.advance(16ms);  // t = 50
  strip(Rgb{30, 200, 30});
  apply();
  s.loop.advance(18ms);  // t = 68
  e.at(68, ref::image_bg(look.strip_jpeg(), 0, 0, 800, 112));
  s.loop.advance(12ms);  // t = 80
  key(0, Rgb{250, 250, 0});
  strip(Rgb{200, 200, 30});
  apply();
  s.loop.advance(22ms);  // t = 102
  e.at(80, ref::image_key(ref::key_index(0), look.key(0)));
  e.at(80, ref::command("STP"));
  e.at(102, ref::image_bg(look.strip_jpeg(), 0, 0, 800, 112));

  // Rings: brightness 0, or four black rings, send LBLIG 0 alone (no SETLB).
  s.loop.advance(8ms);  // t = 110
  cfg.ring_brightness = 0;
  apply();
  e.at(110, ref::command("LBLIG", {0}));
  cfg.ring_brightness = 80;
  cfg.ring_colors = {Rgb{0, 0, 0}, Rgb{0, 0, 0}, Rgb{0, 0, 0}, Rgb{0, 0, 0}};
  apply();
  e.at(110, ref::command("LBLIG", {0}));
  cfg.ring_colors = default_rings;
  apply();
  expect_rings(e, 110, 80);

  // Sleep drops a queued frame: key 0 is queued at 120 (flush due at 120) but never sent.
  s.loop.advance(10ms);  // t = 120
  key(0, Rgb{0, 250, 250});
  apply();
  s.deck.suspend();
  s.dm.sleep();
  e.at(120, ref::command("HAN"));
  e.at(120, ref::command("LBLIG", {0}));
  // Wake at 130: the driver keeps its pacing, so keys (last 80) go at 130, the strip (last 102) at 136.
  s.loop.advance(10ms);  // t = 130
  s.deck.resume();
  s.dm.wake();
  s.loop.drain();
  s.loop.advance(10ms);  // t = 140
  expect_init(e, 130, 70);
  expect_rings(e, 130, 80);
  expect_rings(e, 130, 80);
  for (int i = 0; i < 10; ++i) e.at(130, ref::image_key(ref::key_index(i), look.key(i)));
  e.at(130, ref::command("STP"));
  e.at(136, ref::image_bg(look.strip_jpeg(), 0, 0, 800, 112));

  // Write failure mid-strip at 200: the third write fails, nothing more is written to that link,
  // the queued key frame is dropped, and the device reconnects after kRetry (2000 ms).
  s.loop.advance(60ms);  // t = 200
  REQUIRE(s.link);
  s.link->fail_at = s.sent.size() + 2;
  key(1, Rgb{250, 0, 250});
  strip(Rgb{30, 200, 200});
  apply();
  s.loop.advance(1ms);  // t = 201: the flush ran at 200
  {
    const std::vector<Bytes> bg = ref::image_bg(look.strip_jpeg(), 0, 0, 800, 112);
    REQUIRE(bg.size() > 3);
    for (std::size_t i = 0; i < 3; ++i) e.at(200, bg[i]);
  }
  s.loop.advance(1999ms);  // t = 2200: reconnected
  expect_reconnect(e, 2200, 70, look);

  // Read error at 2300 with a key frame queued (due at 2300, not yet sent): the frame is dropped,
  // the link closed, and the device reconnects at 4300 showing the new frame.
  s.loop.advance(100ms);  // t = 2300
  key(2, Rgb{120, 120, 120});
  apply();
  REQUIRE(s.link);
  REQUIRE(s.link->reads.empty());
  s.loop.signal(s.link);
  s.loop.advance(2000ms);  // t = 4300
  expect_reconnect(e, 4300, 70, look);
  s.dm.shutdown();
  e.at(4300, ref::command("CLE\0\0DC"sv));
  e.at(4300, ref::command("HAN"));

  compare(s.sent, e);
}
