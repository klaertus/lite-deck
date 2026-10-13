#include <doctest/doctest.h>
#include <algorithm>
#include <deque>
#include <array>
#include "device/device_manager.h"
#include "device/registry.h"
#include "fake_loop.h"
#include "image_util.h"
#include "test_util.h"

using namespace ld;
using namespace std::chrono_literals;

namespace {
struct FakeLink : DeviceLink {
  std::vector<std::string>* log;
  bool fail_writes = false;
  std::deque<std::vector<uint8_t>> reads;
  bool read_error = false;
  bool* destroyed = nullptr;
  explicit FakeLink(std::vector<std::string>* l) : log(l) {}
  ~FakeLink() override {
    if (destroyed) *destroyed = true;
  }
  bool write(std::span<const std::uint8_t> r) override {
    if (fail_writes) return false;
    log->push_back(report_tag(r));
    return true;
  }
  void* read_event() override { return this; }
  bool start_read() override { return true; }
  std::optional<std::vector<uint8_t>> finish_read() override {
    if (read_error || reads.empty()) return std::nullopt;
    auto r = reads.front();
    reads.pop_front();
    return r;
  }
};

struct Harness {
  FakeLoop loop;
  std::vector<std::string> sent;
  FakeLink* link = nullptr;
  bool other_app = false;
  int opens = 0;
  bool device_present = true;
  bool link_destroyed = false;
  DeviceManager mgr{loop, default_device(),
                    [this]() -> std::unique_ptr<DeviceLink> {
                      ++opens;
                      if (!device_present) return nullptr;
                      auto l = std::make_unique<FakeLink>(&sent);
                      l->destroyed = &link_destroyed;
                      link = l.get();
                      return l;
                    },
                    [this] { return other_app; }};
};
}  // namespace

TEST_CASE("DeviceManager connects, initializes and notifies") {
  Harness h;
  int connected = 0;
  h.mgr.on_connected = [&] { ++connected; };
  h.mgr.start();
  CHECK(h.mgr.state() == DeviceManager::State::Connected);
  CHECK(connected == 1);
  REQUIRE(h.sent.size() >= 1);
  CHECK(h.sent[0] == "DIS");
}

TEST_CASE("DeviceManager stays busy while another app runs") {
  Harness h;
  h.other_app = true;
  h.mgr.start();
  CHECK(h.mgr.state() == DeviceManager::State::Busy);
  CHECK(h.opens == 0);
  h.loop.advance(2100ms);
  CHECK(h.opens == 0);
  h.other_app = false;
  h.loop.advance(2100ms);
  CHECK(h.mgr.state() == DeviceManager::State::Connected);
}

TEST_CASE("DeviceManager releases the device when another app starts and takes it back after") {
  Harness h;
  int connected = 0;
  std::vector<DeviceManager::State> states;
  h.mgr.on_connected = [&] { ++connected; };
  h.mgr.on_state = [&](DeviceManager::State s) { states.push_back(s); };
  h.mgr.start();
  REQUIRE(h.mgr.state() == DeviceManager::State::Connected);
  CHECK(connected == 1);
  h.other_app = true;
  h.loop.advance(2100ms);
  CHECK(h.mgr.state() == DeviceManager::State::Busy);
  CHECK(h.link_destroyed);
  h.sent.clear();
  h.loop.advance(4100ms);
  CHECK(h.sent.empty());
  CHECK(h.mgr.state() == DeviceManager::State::Busy);
  h.other_app = false;
  h.loop.advance(2100ms);
  CHECK(h.mgr.state() == DeviceManager::State::Connected);
  CHECK(connected == 2);
  CHECK(states == std::vector<DeviceManager::State>{DeviceManager::State::Connected, DeviceManager::State::Busy,
                                                    DeviceManager::State::Connected});
}

TEST_CASE("DeviceManager polls every 30 s while the device is absent and connects soon after an arrival") {
  Harness h;
  h.device_present = false;
  h.mgr.start();
  CHECK(h.mgr.state() == DeviceManager::State::Disconnected);
  CHECK(h.opens == 1);
  h.loop.advance(29000ms);
  CHECK(h.opens == 1);
  h.loop.advance(1100ms);
  CHECK(h.opens == 2);
  h.device_present = true;
  h.mgr.device_arrived();
  h.loop.advance(400ms);
  CHECK(h.opens == 2);
  h.loop.advance(200ms);
  CHECK(h.opens == 3);
  CHECK(h.mgr.state() == DeviceManager::State::Connected);
}

TEST_CASE("DeviceManager retries fast five times after an arrival, then falls back to 30 s") {
  Harness h;
  h.device_present = false;
  h.mgr.start();                 // open #1 at t=0
  h.mgr.device_arrived();        // cancels the 30 s retry
  h.loop.advance(11000ms);       // opens at 0.5, 2.5, 4.5, 6.5, 8.5, 10.5 s
  CHECK(h.opens == 7);
  h.loop.advance(29000ms);       // next one at 40.5 s
  CHECK(h.opens == 7);
  h.loop.advance(1000ms);
  CHECK(h.opens == 8);
}

TEST_CASE("DeviceManager ignores arrivals while connected or while another app holds the device") {
  Harness h;
  h.mgr.start();
  REQUIRE(h.mgr.state() == DeviceManager::State::Connected);
  h.mgr.device_arrived();
  h.loop.advance(1000ms);
  CHECK(h.opens == 1);

  Harness b;
  b.other_app = true;
  b.mgr.start();
  b.mgr.device_arrived();
  b.loop.advance(600ms);
  CHECK(b.opens == 0);
  CHECK(b.mgr.state() == DeviceManager::State::Busy);
}

TEST_CASE("DeviceManager reconnects after write failure") {
  Harness h;
  int connected = 0;
  h.mgr.on_connected = [&] { ++connected; };
  h.mgr.start();
  h.link->fail_writes = true;
  h.mgr.key_image(0, SolidImage(112, 112, Rgb{1, 1, 1}).image());
  h.loop.advance(50ms);  // flush -> write fails -> disconnect
  CHECK(h.mgr.state() == DeviceManager::State::Disconnected);
  h.loop.advance(2000ms);
  CHECK(h.mgr.state() == DeviceManager::State::Connected);
  CHECK(connected == 2);
}

TEST_CASE("DeviceManager decodes reads into input events and ignores write acks") {
  Harness h;
  std::vector<DeviceInput> inputs;
  h.mgr.on_input = [&](const DeviceInput& in) { inputs.push_back(in); };
  h.mgr.start();
  h.link->reads.push_back(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 ff 00"));
  h.loop.signal(h.link);
  h.link->reads.push_back(report_from_hex("00 41 43 4b 00 00 4f 4b 00 00 03 01"));
  h.loop.signal(h.link);
  REQUIRE(inputs.size() == 1);
  CHECK(inputs[0].kind == InputKind::KeyDown);
  CHECK(inputs[0].index == 2);
}

TEST_CASE("DeviceManager disconnects on read error") {
  Harness h;
  h.mgr.start();
  h.link->read_error = true;
  h.loop.signal(h.link);
  CHECK(h.mgr.state() == DeviceManager::State::Disconnected);
}

TEST_CASE("DeviceManager sends heartbeat every 10 s and paces images") {
  Harness h;
  h.mgr.start();
  h.sent.clear();
  h.loop.advance(10000ms);
  CHECK(std::count(h.sent.begin(), h.sent.end(), "CONNECT") == 1);
  h.sent.clear();
  h.mgr.strip_image(SolidImage(800, 112, Rgb{1, 1, 1}).image());
  h.mgr.strip_image(SolidImage(800, 112, Rgb{2, 2, 2}).image());
  h.loop.advance(1ms);
  CHECK(std::count(h.sent.begin(), h.sent.end(), "BGPIC") == 1);
}

TEST_CASE("DeviceManager reapplies cached rings on reconnect and goes dark on sleep") {
  Harness h;
  const std::array<Rgb, 4> red{Rgb{255, 0, 0}, Rgb{}, Rgb{}, Rgb{}};
  h.mgr.rings(red, 100);  // before connect: cached only
  h.mgr.start();
  CHECK(std::count(h.sent.begin(), h.sent.end(), "SETLB") == 1);
  h.sent.clear();
  h.mgr.sleep();
  CHECK(h.sent == std::vector<std::string>{"HAN", "LBLIG"});
  h.sent.clear();
  h.mgr.wake();
  CHECK(h.sent[0] == "DIS");
  CHECK(std::count(h.sent.begin(), h.sent.end(), "SETLB") == 1);
}

TEST_CASE("DeviceManager survives a failing write inside on_state while asleep") {
  Harness h;
  h.mgr.sleep();
  h.mgr.on_state = [&](DeviceManager::State s) {
    if (s == DeviceManager::State::Connected) {
      h.link->fail_writes = true;
      h.mgr.set_brightness(10);
    }
  };
  h.mgr.start();
  CHECK(h.mgr.state() == DeviceManager::State::Disconnected);
}

TEST_CASE("DeviceManager destructor does not invoke callbacks") {
  int calls = 0;
  {
    FakeLoop loop;
    std::vector<std::string> sent;
    DeviceManager mgr{loop, default_device(), [&]() -> std::unique_ptr<DeviceLink> { return std::make_unique<FakeLink>(&sent); },
                      [] { return false; }};
    mgr.start();
    mgr.on_state = [&](DeviceManager::State) { ++calls; };
  }
  CHECK(calls == 0);
}

TEST_CASE("DeviceManager sends nothing after HAN when images were queued before sleep") {
  Harness h;
  h.mgr.start();
  h.sent.clear();
  h.mgr.key_image(0, SolidImage(112, 112, Rgb{1, 1, 1}).image());
  h.mgr.sleep();  // before the flush timer fires
  h.loop.advance(100ms);
  CHECK(h.sent == std::vector<std::string>{"HAN", "LBLIG"});
}

TEST_CASE("DeviceManager shutdown sends CLE then HAN and never reconnects") {
  Harness h;
  h.mgr.start();
  h.sent.clear();
  h.mgr.shutdown();
  CHECK(h.sent == std::vector<std::string>{"CLE", "HAN"});
  CHECK(h.mgr.state() == DeviceManager::State::Disconnected);
  int opens = h.opens;
  h.loop.advance(10000ms);
  CHECK(h.opens == opens);
}

TEST_CASE("DeviceManager coalesces a burst of arrivals into one attempt") {
  Harness h;
  h.device_present = false;
  h.mgr.start();
  h.mgr.device_arrived();
  h.loop.advance(40ms);
  h.mgr.device_arrived();
  h.loop.advance(40ms);
  h.mgr.device_arrived();
  h.loop.advance(400ms);
  CHECK(h.opens == 1);
  h.loop.advance(100ms);  // 500 ms after the last arrival
  CHECK(h.opens == 2);
}

TEST_CASE("DeviceManager retries fast five times after a lost link, then falls back to 30 s") {
  Harness h;
  h.mgr.start();
  REQUIRE(h.opens == 1);
  h.device_present = false;
  h.link->fail_writes = true;
  h.mgr.key_image(0, SolidImage(112, 112, Rgb{1, 1, 1}).image());
  h.loop.advance(50ms);  // write fails -> disconnect, retries at +2, +4, +6, +8, +10 s
  CHECK(h.mgr.state() == DeviceManager::State::Disconnected);
  h.loop.advance(10100ms);
  CHECK(h.opens == 6);
  h.loop.advance(29000ms);
  CHECK(h.opens == 6);
  h.loop.advance(1500ms);
  CHECK(h.opens == 7);
}

TEST_CASE("matches_usb_path matches the VID and PID of a data sheet, any case") {
  const UsbMatch n4 = default_device().layout.usb;
  CHECK(matches_usb_path(L"\\\\?\\HID#VID_5548&PID_1021&MI_00#7&abc#{4d1e55b2}", n4));
  CHECK(matches_usb_path(L"\\\\?\\hid#vid_5548&pid_1021#x", n4));
  CHECK_FALSE(matches_usb_path(L"\\\\?\\USB#VID_0781&PID_5567#x", n4));
  CHECK_FALSE(matches_usb_path(L"", n4));
  CHECK(matches_usb_path(L"\\\\?\\HID#VID_FFFF&PID_FFFE#x", UsbMatch{0xFFFF, 0xFFFE}));
}
