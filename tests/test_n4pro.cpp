#include <doctest/doctest.h>
#include <array>
#include "device/n4pro/n4pro_device.h"
#include "n4pro_reference.h"
#include "render/renderer.h"
#include "test_util.h"

using namespace ld;
using namespace ld::n4pro;

namespace {
struct RecordingTransport : Transport {
  std::vector<std::vector<uint8_t>> sent;
  int fail_after = -1;  // fail the Nth write (0-based); -1 = never
  bool write(std::span<const std::uint8_t> r) override {
    if (fail_after >= 0 && static_cast<int>(sent.size()) >= fail_after) return false;
    sent.emplace_back(r.begin(), r.end());
    return true;
  }
  std::vector<std::string> tags() const {
    std::vector<std::string> t;
    for (const auto& r : sent) t.push_back(report_tag(r));
    return t;
  }
};
TimePoint T(int ms) { return TimePoint{} + Duration(ms); }
std::vector<uint8_t> jpeg(uint8_t fill) { return std::vector<uint8_t>(10, fill); }
}  // namespace

TEST_CASE("initialize wakes, sets brightness and clears both layers") {
  RecordingTransport t;
  N4Pro d(t);
  d.initialize(60);
  CHECK(t.tags() == std::vector<std::string>{"DIS", "LIG", "CLE", "BGCLE", "STP"});
  CHECK(t.sent[1][11] == 60);
}

TEST_CASE("key images are batched with a single STP and the latest image wins") {
  RecordingTransport t;
  N4Pro d(t);
  d.queue_key(0, jpeg(1));
  d.queue_key(0, jpeg(2));  // replaces the first
  d.queue_key(7, jpeg(3));
  REQUIRE(d.next_flush(T(0)) == T(0));
  d.flush(T(0));
  CHECK(t.tags() == std::vector<std::string>{"BAT", "", "BAT", "", "STP"});
  CHECK(t.sent[0][13] == 11);    // slot 0 -> index 11
  CHECK(t.sent[1][1] == 2);      // latest image data
  CHECK(t.sent[2][13] == 8);     // slot 7 -> index 8
  CHECK_FALSE(d.next_flush(T(0)));
}

TEST_CASE("strip frames go out as BGPIC 800x112 and are paced to 34 ms") {
  RecordingTransport t;
  N4Pro d(t);
  d.queue_strip(jpeg(1));
  d.flush(T(0));
  CHECK(t.tags() == std::vector<std::string>{"BGPIC", ""});
  d.queue_strip(jpeg(2));
  d.queue_strip(jpeg(3));
  CHECK(d.next_flush(T(10)) == T(34));
  d.flush(T(10));  // too early: nothing sent
  CHECK(t.sent.size() == 2);
  d.flush(T(34));
  CHECK(t.sent.size() == 4);
  CHECK(t.sent[3][1] == 3);  // intermediate frame dropped
}

TEST_CASE("set_rings uses LBLIG 0 when every ring is black") {
  RecordingTransport t;
  N4Pro d(t);
  d.set_rings({Rgb{}, Rgb{}, Rgb{}, Rgb{}}, 100);
  CHECK(t.tags() == std::vector<std::string>{"LBLIG"});
  CHECK(t.sent[0][11] == 0);
  t.sent.clear();
  d.set_rings({Rgb{255, 0, 0}, Rgb{}, Rgb{}, Rgb{}}, 50);
  CHECK(t.tags() == std::vector<std::string>{"LBLIG", "SETLB"});
  CHECK(t.sent[0][11] == 50);
  CHECK(t.sent[1][11] == 255);
}

TEST_CASE("heartbeat, sleep and shutdown send the expected commands") {
  RecordingTransport t;
  N4Pro d(t);
  d.heartbeat();
  d.sleep();
  d.shutdown();
  CHECK(t.tags() == std::vector<std::string>{"CONNECT", "HAN", "LBLIG", "CLE", "HAN"});
}

TEST_CASE("a failed write marks the driver failed and stops further writes") {
  RecordingTransport t;
  t.fail_after = 1;
  N4Pro d(t);
  d.initialize(60);
  CHECK(d.failed());
  CHECK(t.sent.size() == 1);
  d.heartbeat();
  CHECK(t.sent.size() == 1);
}

TEST_CASE("sleep drops queued images") {
  RecordingTransport t;
  N4Pro d(t);
  d.queue_key(0, jpeg(1));
  d.queue_strip(jpeg(2));
  d.sleep();
  CHECK_FALSE(d.next_flush(T(0)));
  t.sent.clear();
  d.flush(T(100));
  CHECK(t.sent.empty());
}

TEST_CASE("N4ProDevice queues JPEG frames, keys at 85 and the strip at 80, turned 180 degrees") {
  RecordingTransport t;
  n4pro::N4ProDevice d(t);
  Renderer r;
  Surface key(r, 112, 112), strip(r, 800, 112);
  for (Surface* s : {&key, &strip}) {
    Canvas c = s->canvas();
    c.clear(Rgb{0, 0, 0});
    c.fill_rect(RectF{0, 0, 30, 40}, Rgb{255, 255, 255});
  }
  {
    SurfacePixels px(key);
    d.key_image(3, px.image());
  }
  {
    SurfacePixels px(strip);
    d.strip_image(px.image());
  }
  d.flush(T(0));
  std::vector<ref::Bytes> want = ref::image_bg(ref::jpeg(r, strip, 80, true), 0, 0, 800, 112);
  for (const ref::Bytes& rep : ref::image_key(ref::key_index(3), ref::jpeg(r, key, 85, true))) want.push_back(rep);
  want.push_back(ref::command("STP"));
  CHECK(t.sent == want);
}

TEST_CASE("N4ProDevice sends the first four ring colours (missing ones black) and decodes through the protocol") {
  RecordingTransport t;
  n4pro::N4ProDevice d(t);
  const std::array<Rgb, 2> two{Rgb{255, 0, 0}, Rgb{0, 255, 0}};
  d.set_rings(two, 50);
  REQUIRE(t.sent.size() == 2);
  CHECK(t.sent[1] == ref::command("SETLB", {255, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(d.decode_input(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 03 01")) == DeviceInput{InputKind::KeyDown, 2});
  CHECK(d.decode_input(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 ff 00")).kind == InputKind::None);
}
