// src/device/device.h: what a device driver provides (spec "device profiles" §2). A driver lives in
// src/device/<model>/ and is listed in src/device/registry.cpp; the core only sees this contract.
#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include "core/image.h"
#include "core/types.h"
#include "device/transport.h"

namespace ld {

// Bounds of a data sheet (registry.cpp checks them): they keep the surfaces within the memory budget.
inline constexpr int kMaxDeviceKeys = 64;
inline constexpr int kMaxDeviceKnobs = 16;
inline constexpr int kMaxKeySide = 512;        // px
inline constexpr int kMaxStripWidth = 4096;    // px
inline constexpr int kMaxStripHeight = 512;    // px
inline constexpr std::size_t kMaxDeviceIdLength = 64;
inline constexpr Duration kMinHeartbeat{100};
inline constexpr Duration kMaxHeartbeat{60000};
inline constexpr std::size_t kMaxVendorAppLength = 64;

struct PixelSize {
  int w = 0, h = 0;
  bool operator==(const PixelSize&) const = default;
};

// How the device is found on USB. A HID interface matches when its VID and PID match, its usage page
// matches (when set) and its output report has `output_report` bytes, report id included (when not 0).
struct UsbMatch {
  std::uint16_t vid = 0, pid = 0;
  std::optional<std::uint16_t> usage_page;
  std::uint16_t output_report = 0;
  bool operator==(const UsbMatch&) const = default;
};

// The data sheet of a device model. Keys are numbered left to right, then top to bottom.
struct DeviceLayout {
  std::string name;  // shown to the user, e.g. "MiraBox N4 Pro E"
  std::string id;    // stable, written in config.json (device.model), e.g. "mirabox-n4pro"
  UsbMatch usb;
  int rows = 0, cols = 0;
  PixelSize key_size;
  std::optional<PixelSize> strip;  // the touch strip, if any
  int knobs = 0;
  bool knob_rings = false;  // RGB rings around the knobs
  // How often Device::heartbeat() is called while connected (the device's keep-alive).
  Duration heartbeat{10000};
  // The vendor application that takes the device when it runs, e.g. "StreamDock.exe" ("" = none):
  // while it runs, lite-deck releases the device and shows "<name without .exe> active".
  std::string vendor_app;
  int key_count() const { return rows * cols; }
  bool operator==(const DeviceLayout&) const = default;
};

// The vendor app as shown to the user: `vendor_app` without ".exe" ("StreamDock"); "" = none.
inline std::string vendor_app_label(const DeviceLayout& l) {
  std::string name = l.vendor_app;
  if (name.size() > 4) {
    std::string tail = name.substr(name.size() - 4);
    for (char& ch : tail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (tail == ".exe") name.resize(name.size() - 4);
  }
  return name;
}

enum class InputKind { None, KeyDown, KeyUp, KnobTurn, KnobPress, KnobRelease, TouchDown, TouchMove, TouchUp };

// One input in device terms. `index`: key slot or knob; `delta` for KnobTurn: the signed number of
// notches turned (negative counter-clockwise, positive clockwise; may be more than 1 at once); x, y:
// touch position in strip pixels (TouchUp may carry 0).
struct DeviceInput {
  InputKind kind = InputKind::None;
  int index = 0;
  int delta = 0;
  int x = 0;
  int y = 0;
  bool operator==(const DeviceInput&) const = default;
};

// A connected device, on the loop thread only. The driver encodes and paces its own output:
// key_image / strip_image queue a frame, and the owner calls flush() at next_flush().
class Device {
public:
  virtual ~Device() = default;
  virtual void initialize(std::uint8_t brightness) = 0;
  virtual void set_brightness(std::uint8_t pct) = 0;
  virtual void heartbeat() = 0;
  virtual void sleep() = 0;  // drops queued frames, darkens the device
  virtual void shutdown() = 0;
  // `image` has the layout's key (or strip) size and is lent for the call only.
  virtual void key_image(int slot, const Image& image) = 0;
  virtual void strip_image(const Image& image) = 0;
  // One colour per knob; only called when the layout has knob rings.
  virtual void set_rings(std::span<const Rgb> colors, std::uint8_t brightness) = 0;
  // One input report as read from the device (report id first); None for anything else.
  virtual DeviceInput decode_input(std::span<const std::uint8_t> report) = 0;
  virtual std::optional<TimePoint> next_flush(TimePoint now) const = 0;
  virtual void flush(TimePoint now) = 0;
  virtual bool failed() const = 0;  // a write failed: the owner reconnects
};

// One driver: its data sheet and how to make it on an open transport. The owner (DeviceManager)
// calls `factory` on the loop thread each time it opens the device's HID link, then initialize();
// nullptr counts as a failed connection (logged, retried). The Transport outlives the Device made on
// it, which is destroyed on every disconnect before the link closes: the driver may keep a reference.
struct DeviceEntry {
  DeviceLayout layout;
  std::function<std::unique_ptr<Device>(Transport&)> factory;
};

}  // namespace ld
