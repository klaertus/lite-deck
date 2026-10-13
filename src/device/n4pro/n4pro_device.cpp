// src/device/n4pro/n4pro_device.cpp
#include "device/n4pro/n4pro_device.h"
#include <array>

namespace ld::n4pro {

const DeviceLayout& layout() {
  static const DeviceLayout l{
      .name = "MiraBox N4 Pro E",
      .id = "mirabox-n4pro",
      .usb = UsbMatch{.vid = 0x5548, .pid = 0x1021, .usage_page = std::uint16_t{0xFFA0},
                      .output_report = static_cast<std::uint16_t>(proto::kReportOut)},
      .rows = 2,
      .cols = 5,
      .key_size = {112, 112},
      .strip = PixelSize{800, 112},
      .knobs = 4,
      .knob_rings = true,
      .heartbeat = Duration{10000},
      .vendor_app = "StreamDock.exe"};
  return l;
}

DeviceEntry entry() {
  return DeviceEntry{layout(), [](Transport& t) -> std::unique_ptr<Device> { return std::make_unique<N4ProDevice>(t); }};
}

void N4ProDevice::key_image(int slot, const Image& image) { n4_.queue_key(slot, jpeg_.encode(image, kKeyQuality, kOrientation)); }

void N4ProDevice::strip_image(const Image& image) { n4_.queue_strip(jpeg_.encode(image, kStripQuality, kOrientation)); }

void N4ProDevice::set_rings(std::span<const Rgb> colors, std::uint8_t brightness) {
  std::array<Rgb, 4> four{};
  for (std::size_t i = 0; i < four.size() && i < colors.size(); ++i) four[i] = colors[i];
  n4_.set_rings(four, brightness);
}

DeviceInput N4ProDevice::decode_input(std::span<const std::uint8_t> report) {
  return proto::to_device_input(proto::decode(report));
}

}  // namespace ld::n4pro
