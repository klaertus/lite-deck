// tests/test_device_registry.cpp: data sheet checks, the registry, the Test Deck driver.
#include <doctest/doctest.h>
#include <array>
#include <string>
#include <vector>
#include "device/registry.h"
#include "fake_device.h"

using namespace ld;

namespace {
DeviceEntry entry_of(DeviceLayout l) {
  DeviceEntry e = fake::test_deck_entry();
  e.layout = std::move(l);
  return e;
}
struct RecordingTransport final : Transport {
  std::vector<std::vector<std::uint8_t>> sent;
  bool write(std::span<const std::uint8_t> r) override {
    sent.emplace_back(r.begin(), r.end());
    return true;
  }
};
}  // namespace

TEST_CASE("layout_errors: the Test Deck is valid; every bound of a data sheet is checked") {
  CHECK(layout_errors(fake::test_deck_layout()).empty());
  auto broken = [](auto change) {
    DeviceLayout l = fake::test_deck_layout();
    change(l);
    return layout_errors(l);
  };
  CHECK_FALSE(broken([](DeviceLayout& l) { l.id.clear(); }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.id = std::string(65, 'x'); }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.name.clear(); }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.rows = 0; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.cols = -1; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.rows = 9; l.cols = 8; }).empty());  // 72 keys
  CHECK_FALSE(broken([](DeviceLayout& l) { l.key_size = {0, 96}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.key_size = {96, 513}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.strip = PixelSize{0, 100}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.strip = PixelSize{59, 100}; }).empty());  // narrower than one zone
  CHECK(broken([](DeviceLayout& l) { l.strip = PixelSize{60, 100}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.strip = PixelSize{4097, 100}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.knobs = 17; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.knobs = -1; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.heartbeat = Duration{0}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.heartbeat = Duration{99}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.heartbeat = Duration{60001}; }).empty());
  CHECK(broken([](DeviceLayout& l) { l.heartbeat = Duration{100}; }).empty());
  CHECK_FALSE(broken([](DeviceLayout& l) { l.vendor_app = std::string(65, 'x'); }).empty());
  CHECK(broken([](DeviceLayout& l) { l.vendor_app = "Vendor.exe"; }).empty());
  CHECK(broken([](DeviceLayout& l) { l.rows = 8; l.cols = 8; l.knobs = 16; l.strip = PixelSize{4096, 512}; }).empty());
}

TEST_CASE("sound_devices drops an invalid data sheet, a missing factory and a repeated id, keeping the order") {
  DeviceLayout bad = fake::test_deck_layout();
  bad.id = "bad";
  bad.rows = 0;
  DeviceLayout other = fake::test_deck_layout();
  other.id = "other";
  DeviceEntry no_factory = entry_of(fake::test_deck_layout());
  no_factory.layout.id = "no-factory";
  no_factory.factory = nullptr;
  const auto kept = sound_devices({entry_of(fake::test_deck_layout()), entry_of(bad), entry_of(other), no_factory,
                                   entry_of(fake::test_deck_layout())});
  REQUIRE(kept.size() == 2);
  CHECK(kept[0].layout.id == "test-deck");
  CHECK(kept[1].layout.id == "other");
}

TEST_CASE("the Test Deck driver writes one report per call and decodes its input reports") {
  RecordingTransport t;
  fake::test_deck_calls().clear();
  auto dev = fake::test_deck_entry().factory(t);
  dev->initialize(60);
  const std::vector<std::uint8_t> px(96 * 96 * 4, 0);
  dev->key_image(12, Image{96, 96, 96 * 4, px});
  const std::array<Rgb, 2> rings{};
  dev->set_rings(rings, 50);
  CHECK(fake::test_deck_calls() == std::vector<std::string>{"init 60", "key 12 96x96", "rings 2 50"});
  CHECK(t.sent.size() == 3);
  CHECK(dev->decode_input(fake::test_deck_report('D', 12)) == DeviceInput{InputKind::KeyDown, 12});
  CHECK(dev->decode_input(fake::test_deck_report('T', 1, -1)) == DeviceInput{InputKind::KnobTurn, 1, -1});
  CHECK(dev->decode_input(fake::test_deck_report('R', 0)) == DeviceInput{InputKind::KnobRelease, 0});
  CHECK(dev->decode_input(fake::test_deck_report('?', 0)).kind == InputKind::None);
  CHECK(dev->decode_input(std::vector<std::uint8_t>{0}).kind == InputKind::None);
  CHECK_FALSE(dev->failed());
}

TEST_CASE("the registry holds the N4 Pro with its data sheet, and it is the default") {
  REQUIRE(find_device("mirabox-n4pro") != nullptr);
  const DeviceLayout& l = default_device().layout;
  CHECK(l.id == "mirabox-n4pro");
  CHECK(l.name == "MiraBox N4 Pro E");
  CHECK(l.rows == 2);
  CHECK(l.cols == 5);
  CHECK(l.key_size == PixelSize{112, 112});
  CHECK(l.strip == PixelSize{800, 112});
  CHECK(l.knobs == 4);
  CHECK(l.knob_rings);
  CHECK(l.usb == UsbMatch{0x5548, 0x1021, std::uint16_t{0xFFA0}, 1025});
  CHECK(l.heartbeat == Duration{10000});
  CHECK(l.vendor_app == "StreamDock.exe");
  CHECK(vendor_app_label(l) == "StreamDock");
  CHECK(layout_errors(l).empty());
  CHECK(find_device("test-deck") == nullptr);  // the Test Deck only exists in the tests
  CHECK(find_device("test-deck", {fake::test_deck_entry()}) != nullptr);
}

TEST_CASE("config_shape: counts, name and strip width of a data sheet; the default shape is the N4 Pro") {
  CHECK(config_shape(default_device().layout) == ConfigShape{});
  CHECK(default_device().layout.id == kDefaultDevice);
  const ConfigShape t = config_shape(fake::test_deck_layout());
  CHECK(t.device == "test-deck");
  CHECK(t.name == "Test Deck");
  CHECK(t.keys == 15);
  CHECK(t.knobs == 2);
  CHECK(t.strip_width == kDefaultStripWidth);  // no strip: the section is checked as on an N4 Pro
  CHECK_FALSE(t.has_strip);
  CHECK(config_shape(default_device().layout).has_strip);
}

TEST_CASE("choose_device: the plugged device first, then the one the config names, then the N4 Pro") {
  const std::vector<DeviceEntry> entries{default_device(), fake::test_deck_entry()};
  auto none = [](const UsbMatch&) { return false; };
  auto test_deck_plugged = [](const UsbMatch& u) { return u.vid == 0xFFFF && u.pid == 0xFFFE; };
  CHECK(choose_device(entries, test_deck_plugged, "").layout.id == "test-deck");
  CHECK(choose_device(entries, test_deck_plugged, "mirabox-n4pro").layout.id == "test-deck");  // plugged wins
  CHECK(choose_device(entries, none, "test-deck").layout.id == "test-deck");
  CHECK(choose_device(entries, none, "").layout.id == "mirabox-n4pro");
  CHECK(choose_device(entries, none, "elgato-x").layout.id == "mirabox-n4pro");  // unknown here: the default
  CHECK(choose_device(entries, nullptr, "").layout.id == "mirabox-n4pro");
  CHECK(&choose_device(device_registry(), none, "") == &default_device());
}

TEST_CASE("vendor_app_label: the vendor app without .exe; empty without one") {
  DeviceLayout l = fake::test_deck_layout();
  CHECK(vendor_app_label(l).empty());  // the Test Deck has none
  l.vendor_app = "Acme Control.EXE";
  CHECK(vendor_app_label(l) == "Acme Control");
  l.vendor_app = "AcmeHub";
  CHECK(vendor_app_label(l) == "AcmeHub");
  l.vendor_app = ".exe";
  CHECK(vendor_app_label(l) == ".exe");
}
