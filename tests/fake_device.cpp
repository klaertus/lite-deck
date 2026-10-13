// tests/fake_device.cpp
#include "fake_device.h"
#include <algorithm>
#include <format>

namespace fake {

const ld::DeviceLayout& test_deck_layout() {
  static const ld::DeviceLayout layout{.name = "Test Deck",
                                       .id = "test-deck",
                                       .usb = ld::UsbMatch{.vid = 0xFFFF, .pid = 0xFFFE},
                                       .rows = 3,
                                       .cols = 5,
                                       .key_size = {96, 96},
                                       .strip = std::nullopt,
                                       .knobs = 2,
                                       .knob_rings = false};
  return layout;
}

const ld::DeviceEntry& test_deck_entry() {
  static const ld::DeviceEntry entry{test_deck_layout(), [](ld::Transport& t) -> std::unique_ptr<ld::Device> {
                                       return std::make_unique<TestDeckDevice>(t);
                                     }};
  return entry;
}

std::vector<std::string>& test_deck_calls() {
  static std::vector<std::string> calls;
  return calls;
}

std::vector<std::uint8_t> test_deck_report(char op, int index, int delta) {
  return {0, static_cast<std::uint8_t>(op), static_cast<std::uint8_t>(index),
          static_cast<std::uint8_t>(static_cast<std::int8_t>(delta))};
}

void TestDeckDevice::send(const std::string& call) {
  test_deck_calls().push_back(call);
  if (failed_) return;
  std::vector<std::uint8_t> report(65, 0);  // report id + 64 bytes
  std::copy_n(call.begin(), std::min<std::size_t>(call.size(), 64), report.begin() + 1);
  if (!t_.write(report)) failed_ = true;
}

void TestDeckDevice::initialize(std::uint8_t b) { send(std::format("init {}", static_cast<int>(b))); }
void TestDeckDevice::set_brightness(std::uint8_t p) { send(std::format("brightness {}", static_cast<int>(p))); }
void TestDeckDevice::heartbeat() { send("heartbeat"); }
void TestDeckDevice::sleep() { send("sleep"); }
void TestDeckDevice::shutdown() { send("shutdown"); }
void TestDeckDevice::key_image(int slot, const ld::Image& image) {
  send(std::format("key {} {}x{}", slot, image.width, image.height));
}
void TestDeckDevice::strip_image(const ld::Image& image) { send(std::format("strip {}x{}", image.width, image.height)); }
void TestDeckDevice::set_rings(std::span<const ld::Rgb> colors, std::uint8_t b) {
  send(std::format("rings {} {}", colors.size(), static_cast<int>(b)));
}

ld::DeviceInput TestDeckDevice::decode_input(std::span<const std::uint8_t> r) {
  if (r.size() < 4) return {};
  const int index = r[2];
  switch (r[1]) {
    case 'D': return {ld::InputKind::KeyDown, index};
    case 'U': return {ld::InputKind::KeyUp, index};
    case 'T': return {ld::InputKind::KnobTurn, index, static_cast<std::int8_t>(r[3])};
    case 'P': return {ld::InputKind::KnobPress, index};
    case 'R': return {ld::InputKind::KnobRelease, index};
    default: return {};
  }
}

}  // namespace fake
