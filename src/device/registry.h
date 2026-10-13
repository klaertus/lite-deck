// src/device/registry.h: the compiled-in device drivers (same principle as modules/registry.cpp).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "core/config.h"
#include "device/device.h"

namespace ld {

// Why `layout` cannot be used (empty = usable): id and name, at least one key, the bounds of device.h.
std::vector<std::string> layout_errors(const DeviceLayout& layout);
// `entries` without the invalid ones, those without a factory and the repeated ids; each one dropped
// is logged.
std::vector<DeviceEntry> sound_devices(std::vector<DeviceEntry> entries);

// The drivers compiled in, checked once by sound_devices. To add one: docs/adding-a-device.md.
const std::vector<DeviceEntry>& device_registry();
// The N4 Pro: the device used when none is plugged in and the config names none.
const DeviceEntry& default_device();
// The entry with this id in `entries`, or nullptr.
const DeviceEntry* find_device(std::string_view id, const std::vector<DeviceEntry>& entries = device_registry());

// The config's view of `layout`: its id and name, its key and knob counts, its strip width
// (kDefaultStripWidth without a strip, so a strip section stays valid and is kept).
ConfigShape config_shape(const DeviceLayout& layout);

// The active device at startup (spec §3): the first entry whose device is plugged in, else the one the
// config names (`configured`; "" = none; an id this build does not know is logged), else the N4 Pro.
// Throws std::logic_error when `entries` is empty.
const DeviceEntry& choose_device(const std::vector<DeviceEntry>& entries,
                                 const std::function<bool(const UsbMatch&)>& plugged, std::string_view configured);

}  // namespace ld
