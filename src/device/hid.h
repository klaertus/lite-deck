// src/device/hid.h
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/types.h"
#include "device/device.h"
#include "device/transport.h"

struct _OVERLAPPED;

namespace ld {

// A connected device: synchronous writes, asynchronous (overlapped) reads.
class DeviceLink : public Transport {
public:
  virtual void* read_event() = 0;  // HANDLE signaled when the pending read completes
  virtual bool start_read() = 0;   // issue the next asynchronous read; false = device gone
  virtual std::optional<std::vector<std::uint8_t>> finish_read() = 0;  // completed report; nullopt = device gone
};

struct HidDeviceInfo {
  std::wstring path;
  std::uint16_t vid = 0, pid = 0, usage_page = 0, in_len = 0, out_len = 0;
};

std::vector<HidDeviceInfo> hid_enumerate(std::uint16_t vid, std::uint16_t pid);
// The interfaces of the present devices that match `usb` (UsbMatch in device/device.h).
std::vector<HidDeviceInfo> hid_candidates(const UsbMatch& usb);
bool hid_present(const UsbMatch& usb);
// True when a WM_DEVICECHANGE interface path (dbcc_name) names the VID and PID of `usb`, any case.
bool matches_usb_path(std::wstring_view path, const UsbMatch& usb);

constexpr Duration kHidWriteTimeout{1000};  // a wedged endpoint must not freeze the loop
constexpr Duration kHidCancelTimeout{500};  // a driver that ignores CancelIoEx must not hang us

// CancelIoEx then a bounded wait for the operation to finish. True once it is no longer pending
// (cancelled or completed); false if it is still pending after `timeout` (its buffers must then leak).
bool cancel_io_bounded(void* handle, _OVERLAPPED* ov, Duration timeout);

// Opens the first openable interface matching `usb`; nullptr when absent or not openable.
std::unique_ptr<DeviceLink> open_hid(const UsbMatch& usb);
// Opens the first of `candidates` that opens (read and write handles, overlapped). A failure is
// logged with its reason, once until the reason changes or an open succeeds; the next candidate is tried.
std::unique_ptr<DeviceLink> open_hid_candidates(const std::vector<HidDeviceInfo>& candidates);

}  // namespace ld
