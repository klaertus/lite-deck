// src/device/registry.cpp
#include "device/registry.h"
#include <format>
#include <set>
#include <stdexcept>
#include "core/log.h"
#include "device/n4pro/n4pro_device.h"

namespace ld {

static_assert(kMaxConfigKeys == static_cast<std::size_t>(kMaxDeviceKeys) &&
              kMaxConfigKnobs == static_cast<std::size_t>(kMaxDeviceKnobs) && kMaxModelLength == kMaxDeviceIdLength &&
              kMaxConfigStripWidth == kMaxStripWidth);

std::vector<std::string> layout_errors(const DeviceLayout& l) {
  std::vector<std::string> e;
  if (l.id.empty()) e.push_back("empty id");
  if (l.id.size() > kMaxDeviceIdLength) e.push_back(std::format("id longer than {} characters", kMaxDeviceIdLength));
  if (l.name.empty()) e.push_back("empty name");
  if (l.rows <= 0 || l.cols <= 0)
    e.push_back(std::format("{} x {} keys: at least one key is needed", l.rows, l.cols));
  else if (l.rows > kMaxDeviceKeys || l.cols > kMaxDeviceKeys || l.rows * l.cols > kMaxDeviceKeys)
    e.push_back(std::format("{} x {} keys: at most {}", l.rows, l.cols, kMaxDeviceKeys));
  if (l.key_size.w <= 0 || l.key_size.h <= 0 || l.key_size.w > kMaxKeySide || l.key_size.h > kMaxKeySide)
    e.push_back(std::format("key size {} x {}: 1 to {} px", l.key_size.w, l.key_size.h, kMaxKeySide));
  if (l.strip && (l.strip->w < kMinZoneWidth || l.strip->h <= 0 || l.strip->w > kMaxStripWidth || l.strip->h > kMaxStripHeight))
    e.push_back(std::format("strip {} x {}: {} to {} px wide (one zone at least), 1 to {} px high", l.strip->w, l.strip->h,
                            kMinZoneWidth, kMaxStripWidth, kMaxStripHeight));
  if (l.knobs < 0 || l.knobs > kMaxDeviceKnobs) e.push_back(std::format("{} knobs: 0 to {}", l.knobs, kMaxDeviceKnobs));
  if (l.heartbeat < kMinHeartbeat || l.heartbeat > kMaxHeartbeat)
    e.push_back(std::format("heartbeat {} ms: {} to {} ms", l.heartbeat.count(), kMinHeartbeat.count(), kMaxHeartbeat.count()));
  if (l.vendor_app.size() > kMaxVendorAppLength)
    e.push_back(std::format("vendor app name longer than {} characters", kMaxVendorAppLength));
  return e;
}

std::vector<DeviceEntry> sound_devices(std::vector<DeviceEntry> entries) {
  std::vector<DeviceEntry> out;
  std::set<std::string> ids;
  for (DeviceEntry& d : entries) {
    std::vector<std::string> errors = layout_errors(d.layout);
    if (!d.factory) errors.push_back("no driver factory");
    if (errors.empty() && !ids.insert(d.layout.id).second) errors.push_back("id used twice");
    if (errors.empty()) {
      out.push_back(std::move(d));
      continue;
    }
    const std::string name = d.layout.id.empty() ? std::string("(no id)") : d.layout.id;
    for (const std::string& e : errors) log_error("device {}: {}; the device is not loaded", name, e);
  }
  return out;
}

const std::vector<DeviceEntry>& device_registry() {
  static const std::vector<DeviceEntry> registry = sound_devices({
      n4pro::entry(),
  });
  return registry;
}

const DeviceEntry* find_device(std::string_view id, const std::vector<DeviceEntry>& entries) {
  for (const DeviceEntry& e : entries)
    if (e.layout.id == id) return &e;
  return nullptr;
}

ConfigShape config_shape(const DeviceLayout& l) {
  return ConfigShape{l.id, l.name, l.key_count(), l.knobs, l.strip ? l.strip->w : kDefaultStripWidth, l.strip.has_value()};
}

const DeviceEntry& default_device() {
  static const DeviceEntry& entry = []() -> const DeviceEntry& {
    const DeviceEntry* e = find_device(n4pro::layout().id);
    if (!e) throw std::logic_error("the N4 Pro data sheet is invalid");  // a test keeps it valid
    return *e;
  }();
  return entry;
}

const DeviceEntry& choose_device(const std::vector<DeviceEntry>& entries,
                                 const std::function<bool(const UsbMatch&)>& plugged, std::string_view configured) {
  if (entries.empty()) throw std::logic_error("no device driver is registered");
  if (plugged)
    for (const DeviceEntry& e : entries)
      if (plugged(e.layout.usb)) return e;
  if (!configured.empty()) {
    if (const DeviceEntry* e = find_device(configured, entries)) return *e;
    std::string shown(configured);  // control characters would forge log lines
    for (char& ch : shown)
      if (static_cast<unsigned char>(ch) < 0x20) ch = '?';
    log_warn("config.json names the device \"{}\", which this build does not know", shown);
  }
  if (const DeviceEntry* e = find_device(kDefaultDevice, entries)) return *e;
  return entries.front();
}

}  // namespace ld
