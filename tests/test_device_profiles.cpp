// tests/test_device_profiles.cpp: the Test Deck (tests/fake_device.*), a device of another shape known
// only to the tests, end to end: config, layout, rendering, input, driver, API, simulator (spec
// "device profiles" §5). The N4 Pro side is pinned by test_n4pro_pinned.cpp and the golden images.
#include <doctest/doctest.h>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <format>
#include "core/config_service.h"
#include "device/device_manager.h"
#include "device/registry.h"
#include "device/simulator.h"
#include "fake_device.h"
#include "fake_loop.h"
#include "fake_web_backend.h"
#include "layout/deck.h"
#include "web/api.h"
#include "web_test_util.h"

using namespace ld;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// The HID link of the Test Deck: writes succeed; reads come from `reads`.
struct TestDeckLink final : DeviceLink {
  std::deque<std::vector<std::uint8_t>> reads;
  bool write(std::span<const std::uint8_t>) override { return true; }
  void* read_event() override { return this; }
  bool start_read() override { return true; }
  std::optional<std::vector<std::uint8_t>> finish_read() override {
    if (reads.empty()) return std::nullopt;
    auto r = reads.front();
    reads.pop_front();
    return r;
  }
};

std::vector<std::string>& events() {
  static std::vector<std::string> e;
  return e;
}
std::vector<std::string>& started() {
  static std::vector<std::string> s;
  return s;
}

const char* kind_name(EventKind k) {
  switch (k) {
    case EventKind::Press: return "Press";
    case EventKind::Release: return "Release";
    case EventKind::Tap: return "Tap";
    case EventKind::LongPress: return "LongPress";
    case EventKind::Rotate: return "Rotate";
    case EventKind::SwipeLeft: return "SwipeLeft";
    case EventKind::SwipeRight: return "SwipeRight";
  }
  return "?";
}

// A module for keys, knobs and zones that records its start and the events it gets, under its name.
struct Recorder final : Module {
  std::string name;
  void start(ModuleContext&, const json& s) override {
    name = s.at("name").get<std::string>();
    started().push_back(name);
  }
  void on_event(const InputEvent& e) override {
    events().push_back(e.kind == EventKind::Rotate ? std::format("{} Rotate {}", name, e.delta)
                                                   : std::format("{} {}", name, kind_name(e.kind)));
  }
  void render(Canvas& c) override { c.clear(Rgb{0, 120, 0}); }
};

const ModuleInfo& rec_info() {
  static const ModuleInfo info = [] {
    ModuleInfo m;
    m.id = "rec";
    m.name = "Recorder";
    m.slots = {SlotKind::Key, SlotKind::Knob, SlotKind::StripZone};
    m.settings_schema = json::parse(R"({"fields":[{"key":"name","type":"string","default":"","label":"Name"}]})");
    m.create = [] { return std::make_unique<Recorder>(); };
    return m;
  }();
  return info;
}
const ModuleInfo* rec_lookup(std::string_view id) { return id == "rec" ? &rec_info() : nullptr; }
json rec(const std::string& name) { return json{{"module", "rec"}, {"settings", {{"name", name}}}}; }

// An N4 Pro config.json: keys k0-k9, knobs n0-n3, one 800 px zone z.
json n4_doc() {
  json keys = json::array(), knobs = json::array();
  for (int i = 0; i < 10; ++i) keys.push_back(rec(std::format("k{}", i)));
  for (int i = 0; i < 4; ++i) knobs.push_back(rec(std::format("n{}", i)));
  json zone = rec("z");
  zone["width"] = 800;
  return json{{"version", 1}, {"keys", keys}, {"knobs", knobs}, {"strip", json::array({zone})}};
}

// What main.cpp wires, on the Test Deck, with config.json in a temp folder.
struct Rig {
  fs::path dir;
  FakeLoop loop;
  Renderer renderer;
  TestDeckLink* link = nullptr;
  ConfigService config;
  DeviceManager dm;
  Deck deck;

  Rig(const char* name, const json& doc)
      : dir(prepare(name, doc)), config(dir / "config.json", rec_lookup, config_shape(fake::test_deck_layout())),
        dm(loop, fake::test_deck_entry(),
           [this]() -> std::unique_ptr<DeviceLink> {
             auto l = std::make_unique<TestDeckLink>();
             link = l.get();
             return l;
           },
           nullptr),
        deck(loop, renderer, dm, fake::test_deck_layout()) {
    events().clear();
    started().clear();
    fake::test_deck_calls().clear();
    dm.on_input = [this](const DeviceInput& in) { deck.on_input(in); };
    dm.on_connected = [this] { deck.redraw_all(); };
    deck.apply(config.load(), rec_lookup);
    loop.drain();
  }
  ~Rig() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  static fs::path prepare(const char* name, const json& doc) {
    const fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    testutil::write_text(d / "config.json", doc.dump());
    return d;
  }
  void read(const std::vector<std::uint8_t>& report) {
    link->reads.push_back(report);
    loop.signal(link);
  }
  json file() const { return json::parse(testutil::read_text(dir / "config.json")); }
};

}  // namespace

TEST_CASE("an N4 Pro config on the Test Deck: keys 0-9 kept, 10-14 empty, the rest kept in the file, not started") {
  Rig rig("ld_profiles_config", n4_doc());
  const Config& c = rig.config.current();
  REQUIRE(c.keys.size() == 15);
  for (int i = 0; i < 10; ++i) {
    INFO(i);
    REQUIRE(c.keys[static_cast<std::size_t>(i)]);
    CHECK(c.keys[static_cast<std::size_t>(i)]->settings["name"] == std::format("k{}", i));
  }
  for (int i = 10; i < 15; ++i) CHECK_FALSE(c.keys[static_cast<std::size_t>(i)]);
  std::vector<std::string> want;
  for (int i = 0; i < 10; ++i) want.push_back(std::format("k{}", i));
  want.push_back("n0");
  want.push_back("n1");
  CHECK(started() == want);  // knobs n2, n3 and the zone z are not started
  const json file = rig.file();
  CHECK(file["device"]["model"] == "test-deck");
  CHECK(file["knobs"][3]["settings"]["name"] == "n3");
  CHECK(file["strip"][0]["settings"]["name"] == "z");
}

TEST_CASE("the Test Deck driver gets 15 frames of 96 x 96 px, no strip and no ring call") {
  Rig rig("ld_profiles_frames", n4_doc());
  rig.dm.start();
  rig.loop.drain();
  const auto& calls = fake::test_deck_calls();
  REQUIRE_FALSE(calls.empty());
  CHECK(calls.front() == "init 60");
  for (int i = 0; i < 15; ++i) {
    INFO(i);
    CHECK(std::count(calls.begin(), calls.end(), std::format("key {} 96x96", i)) == 1);
  }
  CHECK(std::none_of(calls.begin(), calls.end(),
                     [](const std::string& s) { return s.starts_with("strip") || s.starts_with("rings"); }));
}

TEST_CASE("input from the Test Deck reaches the modules: KeyDown(12) on key 12, the two knobs") {
  json doc = n4_doc();
  for (int i = 10; i < 15; ++i) doc["keys"].push_back(nullptr);
  doc["keys"][12] = rec("k12");
  Rig rig("ld_profiles_input", doc);
  rig.dm.start();
  rig.loop.drain();
  events().clear();
  rig.read(fake::test_deck_report('D', 12));
  rig.read(fake::test_deck_report('U', 12));
  rig.read(fake::test_deck_report('T', 1, -1));
  rig.read(fake::test_deck_report('P', 0));
  rig.read(fake::test_deck_report('R', 0));
  rig.read(fake::test_deck_report('D', 15));  // outside the grid: nobody
  rig.read(fake::test_deck_report('P', 2));   // no third knob: nobody
  CHECK(events() == std::vector<std::string>{"k12 Press", "k12 Release", "k12 Tap", "n1 Rotate -1", "n0 Press",
                                             "n0 Release"});
}

TEST_CASE("the API exposes the Test Deck: 3 x 5 keys of 96 px, no strip, 2 knobs without rings") {
  FakeWebBackend backend;
  const std::vector<ModuleInfo> modules{rec_info()};
  WebApi api{"0123456789abcdef0123456789abcdef", backend, modules, nullptr, fake::test_deck_layout()};
  api.set_port(7878);
  auto get = [&](const std::string& path) {
    WebRequest r;
    r.method = "GET";
    r.path = path;
    r.headers["host"] = "127.0.0.1:7878";
    r.headers["cookie"] = "ld=0123456789abcdef0123456789abcdef";
    return api.handle(r);
  };
  const WebResponse r = get("/api/device");
  REQUIRE(r.status == 200);
  CHECK(json::parse(r.body) == json::parse(R"({"id":"test-deck","name":"Test Deck","rows":3,"cols":5,
    "key_size":{"width":96,"height":96},"strip":null,"knobs":2,"knob_rings":false,
    "vendor_app":null})"));
  CHECK(get("/api/preview/key/14").status == 200);
  CHECK(backend.last_preview == 14);
  CHECK(get("/api/preview/key/15").status == 404);
  CHECK(get("/api/preview/strip/0").status == 404);
}

TEST_CASE("on the Test Deck, a strip section is accepted and kept; a module beyond key 14 is refused") {
  Rig rig("ld_profiles_strip", n4_doc());
  json page = rig.config.document();
  json left = rec("left"), right = rec("right");
  left["width"] = 430;
  right["width"] = 370;
  page["strip"] = json::array({left, right});
  page["keys"][14] = rec("k14");
  REQUIRE(rig.config.update(page, 0).status == UpdateStatus::Applied);
  const json file = rig.file();
  REQUIRE(file["strip"].size() == 2);
  CHECK(file["strip"][1]["settings"]["name"] == "right");
  CHECK(file["keys"][14]["settings"]["name"] == "k14");
  page = rig.config.document();
  page["keys"].push_back(rec("k15"));
  const UpdateResult r = rig.config.update(page, 0);
  CHECK(r.status == UpdateStatus::Invalid);
  REQUIRE(r.errors.size() == 1);
  CHECK(r.errors[0].path == "keys[15]");
  CHECK(r.errors[0].message == "the Test Deck has 15 keys");
}

TEST_CASE("the simulator takes the Test Deck: 15 key images, no strip image, no rings") {
  const fs::path dir = fs::temp_directory_path() / "ld_profiles_sim";
  fs::remove_all(dir);
  {
    FakeLoop loop;
    Renderer renderer;
    SimulatorSink sink(dir);
    Deck deck(loop, renderer, sink, fake::test_deck_layout());
    Config c;
    c.keys.resize(15);
    c.keys[14] = SlotConfig{"rec", json{{"name", "k14"}}};
    deck.apply(c, rec_lookup);
    loop.drain();
  }
  for (int i = 0; i < 15; ++i) CHECK(fs::exists(dir / std::format("key{}.jpg", i)));
  CHECK_FALSE(fs::exists(dir / "key15.jpg"));
  CHECK_FALSE(fs::exists(dir / "strip.jpg"));
  CHECK_FALSE(fs::exists(dir / "rings.txt"));
  fs::remove_all(dir);
}

namespace {

struct NullSink final : DeckSink {
  int keys = 0;
  void key_image(int, const Image&) override { ++keys; }
  void strip_image(const Image&) override {}
  void rings(std::span<const Rgb>, std::uint8_t) override {}
};

// A device smaller than the N4 Pro: 2 x 2 keys, one knob without ring, no strip.
DeviceLayout mini_layout() {
  DeviceLayout l;
  l.name = "Mini Deck";
  l.id = "mini-deck";
  l.usb = UsbMatch{0x1234, 0x5679};
  l.rows = 2;
  l.cols = 2;
  l.key_size = PixelSize{96, 96};
  l.knobs = 1;
  return l;
}

}  // namespace

TEST_CASE("an N4 Pro config on a smaller device: keys 4-9, knobs 1-3 and the zone kept in the file, not started") {
  const fs::path dir = Rig::prepare("ld_profiles_mini", n4_doc());
  {
    const DeviceLayout layout = mini_layout();
    ConfigService config(dir / "config.json", rec_lookup, config_shape(layout));
    FakeLoop loop;
    Renderer renderer;
    NullSink sink;
    Deck deck(loop, renderer, sink, layout);
    started().clear();
    events().clear();
    deck.apply(config.load(), rec_lookup);
    loop.drain();
    CHECK(config.current().keys.size() == 10);  // completed, never truncated
    CHECK(started() == std::vector<std::string>{"k0", "k1", "k2", "k3", "n0"});
    CHECK(sink.keys == 4);
    deck.on_input(DeviceInput{InputKind::KeyDown, 3});
    deck.on_input(DeviceInput{InputKind::KeyDown, 5});    // beyond the grid: nobody
    deck.on_input(DeviceInput{InputKind::KnobPress, 3});  // beyond the knobs: nobody
    deck.on_input(DeviceInput{InputKind::TouchDown, 0, 0, 100});
    CHECK(events() == std::vector<std::string>{"k3 Press"});
    const json file = json::parse(testutil::read_text(dir / "config.json"));
    CHECK(file["device"]["model"] == "mini-deck");
    CHECK(file["keys"][9]["settings"]["name"] == "k9");
    CHECK(file["knobs"][3]["settings"]["name"] == "n3");
    CHECK(file["strip"][0]["settings"]["name"] == "z");
  }
  fs::remove_all(dir);
}

TEST_CASE("a 15-key config on the N4 Pro: keys 10-14 kept, not started, not drawn") {
  json doc = n4_doc();
  for (int i = 10; i < 15; ++i) doc["keys"].push_back(rec(std::format("k{}", i)));
  const fs::path dir = Rig::prepare("ld_profiles_n4_15", doc);
  {
    const DeviceLayout& layout = default_device().layout;
    ConfigService config(dir / "config.json", rec_lookup, config_shape(layout));
    FakeLoop loop;
    Renderer renderer;
    NullSink sink;
    Deck deck(loop, renderer, sink, layout);
    started().clear();
    deck.apply(config.load(), rec_lookup);
    loop.drain();
    REQUIRE(config.current().keys.size() == 15);
    std::vector<std::string> want;
    for (int i = 0; i < 10; ++i) want.push_back(std::format("k{}", i));
    for (int i = 0; i < 4; ++i) want.push_back(std::format("n{}", i));
    want.push_back("z");
    CHECK(started() == want);
    CHECK(sink.keys == 10);
    const json file = json::parse(testutil::read_text(dir / "config.json"));
    CHECK(file["keys"][14]["settings"]["name"] == "k14");
  }
  fs::remove_all(dir);
}

TEST_CASE("the DeviceManager calls heartbeat at the interval of the data sheet") {
  DeviceEntry entry = fake::test_deck_entry();
  entry.layout.heartbeat = Duration{3000};
  FakeLoop loop;
  DeviceManager dm(loop, entry, []() -> std::unique_ptr<DeviceLink> { return std::make_unique<TestDeckLink>(); },
                   nullptr);  // no vendor app: never busy
  fake::test_deck_calls().clear();
  dm.start();
  REQUIRE(dm.state() == DeviceManager::State::Connected);
  loop.advance(Duration{9100});
  CHECK(std::count(fake::test_deck_calls().begin(), fake::test_deck_calls().end(), "heartbeat") == 3);
  dm.shutdown();
}
