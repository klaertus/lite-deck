// tests/fake_device.h: "Test Deck", a device that exists only in the tests (spec "device profiles"
// §5): 3 x 5 keys of 96 x 96 px, no strip, 2 knobs without rings. Its driver writes one short report
// per call to its transport and records what it received.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "device/device.h"

namespace fake {

const ld::DeviceLayout& test_deck_layout();
// The registry entry of the Test Deck. Never in device_registry(): tests pass it where they need it.
const ld::DeviceEntry& test_deck_entry();

// Every call any TestDeckDevice received since the last clear(), in order: "init 60",
// "brightness 40", "key 12 96x96", "strip 800x112", "rings 2 50", "heartbeat", "sleep", "shutdown".
std::vector<std::string>& test_deck_calls();

// An input report of the Test Deck: {0, op, index, delta}, op 'D' KeyDown, 'U' KeyUp, 'T' KnobTurn
// (delta as a signed byte), 'P' KnobPress, 'R' KnobRelease. Anything else decodes to None.
std::vector<std::uint8_t> test_deck_report(char op, int index, int delta = 0);

class TestDeckDevice final : public ld::Device {
public:
  explicit TestDeckDevice(ld::Transport& t) : t_(t) {}
  void initialize(std::uint8_t brightness) override;
  void set_brightness(std::uint8_t pct) override;
  void heartbeat() override;
  void sleep() override;
  void shutdown() override;
  void key_image(int slot, const ld::Image& image) override;
  void strip_image(const ld::Image& image) override;
  void set_rings(std::span<const ld::Rgb> colors, std::uint8_t brightness) override;
  ld::DeviceInput decode_input(std::span<const std::uint8_t> report) override;
  std::optional<ld::TimePoint> next_flush(ld::TimePoint) const override { return std::nullopt; }
  void flush(ld::TimePoint) override {}
  bool failed() const override { return failed_; }

private:
  void send(const std::string& call);
  ld::Transport& t_;
  bool failed_ = false;
};

}  // namespace fake
