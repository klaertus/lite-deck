// src/device/n4pro/n4pro_device.h: the MiraBox N4 Pro E driver, on top of the N4Pro command layer.
// Frames become JPEG (keys 85, strip 80), turned 180° because the screen is mounted upside down.
#pragma once
#include "device/device.h"
#include "device/n4pro/n4pro.h"
#include "render/jpeg.h"

namespace ld::n4pro {

inline constexpr int kKeyQuality = 85;
inline constexpr int kStripQuality = 80;
inline constexpr Orientation kOrientation = Orientation::Rotate180;  // the screen is mounted upside down

// 2 x 5 keys of 112 x 112 px, an 800 x 112 strip, 4 knobs with RGB rings; vendor interface
// 0x5548:0x1021, usage page 0xFFA0, 1025-byte output reports.
const DeviceLayout& layout();
DeviceEntry entry();

class N4ProDevice final : public Device {
public:
  explicit N4ProDevice(Transport& transport) : n4_(transport) {}
  void initialize(std::uint8_t brightness) override { n4_.initialize(brightness); }
  void set_brightness(std::uint8_t pct) override { n4_.set_brightness(pct); }
  void heartbeat() override { n4_.heartbeat(); }
  void sleep() override { n4_.sleep(); }
  void shutdown() override { n4_.shutdown(); }
  void key_image(int slot, const Image& image) override;
  void strip_image(const Image& image) override;
  void set_rings(std::span<const Rgb> colors, std::uint8_t brightness) override;
  DeviceInput decode_input(std::span<const std::uint8_t> report) override;
  std::optional<TimePoint> next_flush(TimePoint now) const override { return n4_.next_flush(now); }
  void flush(TimePoint now) override { n4_.flush(now); }
  bool failed() const override { return n4_.failed(); }

private:
  N4Pro n4_;
  JpegEncoder jpeg_;
};

}  // namespace ld::n4pro
