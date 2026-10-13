// src/device/hid.cpp
#include "device/hid.h"
#include "core/log.h"
#include <atomic>
#include <format>
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace ld {

std::vector<HidDeviceInfo> hid_enumerate(std::uint16_t vid, std::uint16_t pid) {
  GUID g;
  HidD_GetHidGuid(&g);
  HDEVINFO info = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE) return {};
  std::vector<HidDeviceInfo> out;
  SP_DEVICE_INTERFACE_DATA d{};
  d.cbSize = sizeof(d);
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &g, i, &d); ++i) {
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &d, nullptr, 0, &need, nullptr);
    if (need == 0) continue;
    std::vector<std::uint8_t> buf(need);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(info, &d, det, need, nullptr, nullptr)) continue;
    HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    HIDD_ATTRIBUTES a{};
    a.Size = sizeof(a);
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetAttributes(h, &a) && a.VendorID == vid && a.ProductID == pid && HidD_GetPreparsedData(h, &pp)) {
      HIDP_CAPS caps{};
      HidP_GetCaps(pp, &caps);
      HidD_FreePreparsedData(pp);
      out.push_back({det->DevicePath, a.VendorID, a.ProductID, caps.UsagePage, caps.InputReportByteLength,
                     caps.OutputReportByteLength});
    }
    CloseHandle(h);
  }
  SetupDiDestroyDeviceInfoList(info);
  return out;
}

bool cancel_io_bounded(void* handle, OVERLAPPED* ov, Duration timeout) {
  CancelIoEx(handle, ov);
  DWORD n = 0;
  GetOverlappedResultEx(handle, ov, &n, static_cast<DWORD>(timeout.count()), FALSE);
  return HasOverlappedIoCompleted(ov);  // the exact "no longer pending" contract, whatever the wait returned
}

namespace {

// One overlapped operation with its own buffer and event. Abandoned (leaked) if the I/O never completes.
struct IoSlot {
  OVERLAPPED ov{};
  std::vector<std::uint8_t> buf;
  ~IoSlot() {
    if (ov.hEvent) CloseHandle(ov.hEvent);
  }
};

std::unique_ptr<IoSlot> make_slot(std::size_t size) {
  auto s = std::make_unique<IoSlot>();
  s->buf.resize(size);
  s->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!s->ov.hEvent) {
    log_error("hid: CreateEventW failed ({})", GetLastError());
    return nullptr;
  }
  return s;
}

constexpr int kMaxLeakedSlots = 4;
std::atomic<int> g_leaked_slots{0};  // process-wide: abandoned slots are never reclaimed

void abandon(std::unique_ptr<IoSlot>& s) {
  const int n = ++g_leaked_slots;
  if (n < kMaxLeakedSlots)
    log_warn("hid: I/O still pending after cancel; leaking its buffer ({})", n);
  else if (n == kMaxLeakedSlots)
    log_error("HID driver ignores cancellation: giving up on the deck until restart");
  (void)s.release();
}

class HidLink final : public DeviceLink {
public:
  HidLink(HANDLE write_h, HANDLE read_h, std::unique_ptr<IoSlot> wslot, std::unique_ptr<IoSlot> rslot)
      : write_(write_h), read_(read_h), wslot_(std::move(wslot)), rslot_(std::move(rslot)) {}
  ~HidLink() override {
    if (pending_ && !cancel_io_bounded(read_, &rslot_->ov, kHidCancelTimeout)) abandon(rslot_);
    CloseHandle(read_);
    CloseHandle(write_);
  }
  bool write(std::span<const std::uint8_t> r) override {
    if (!wslot_) return false;  // a previous write never completed: the link is dead
    IoSlot& s = *wslot_;
    s.buf.assign(r.begin(), r.end());
    ResetEvent(s.ov.hEvent);
    if (!WriteFile(write_, s.buf.data(), static_cast<DWORD>(s.buf.size()), nullptr, &s.ov) &&
        GetLastError() != ERROR_IO_PENDING)
      return false;
    if (WaitForSingleObject(s.ov.hEvent, static_cast<DWORD>(kHidWriteTimeout.count())) != WAIT_OBJECT_0) {
      if (!cancel_io_bounded(write_, &s.ov, kHidCancelTimeout)) abandon(wslot_);
      return false;
    }
    DWORD n = 0;
    return GetOverlappedResult(write_, &s.ov, &n, FALSE) && n == s.buf.size();
  }
  void* read_event() override { return rslot_->ov.hEvent; }
  bool start_read() override {
    ResetEvent(rslot_->ov.hEvent);
    if (ReadFile(read_, rslot_->buf.data(), static_cast<DWORD>(rslot_->buf.size()), nullptr, &rslot_->ov) ||
        GetLastError() == ERROR_IO_PENDING) {
      pending_ = true;
      return true;
    }
    return false;
  }
  std::optional<std::vector<std::uint8_t>> finish_read() override {
    DWORD n = 0;
    pending_ = false;
    if (!GetOverlappedResult(read_, &rslot_->ov, &n, FALSE)) return std::nullopt;
    return std::vector<std::uint8_t>(rslot_->buf.begin(), rslot_->buf.begin() + n);
  }

private:
  HANDLE write_;
  HANDLE read_;
  std::unique_ptr<IoSlot> wslot_;
  std::unique_ptr<IoSlot> rslot_;  // never abandoned while the loop still waits on its event
  bool pending_ = false;
};

// Last open failure that was logged (0 = none since the last success): the daemon retries every
// 30 s, so an unchanged reason is logged once.
std::atomic<DWORD> g_last_open_error{0};

HANDLE open_overlapped(const std::wstring& path) {
  return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                     OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

}  // namespace

std::unique_ptr<DeviceLink> open_hid_candidates(const std::vector<HidDeviceInfo>& candidates) {
  for (const auto& d : candidates) {
    auto wslot = make_slot(d.out_len);
    auto rslot = make_slot(d.in_len);
    if (!wslot || !rslot) return nullptr;  // make_slot logged the error
    HANDLE w = open_overlapped(d.path);
    HANDLE r = w == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE : open_overlapped(d.path);
    if (r == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
      if (g_last_open_error.exchange(err) != err)
        log_warn("hid: cannot open the deck ({}){}", err,
                 err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ? ": another program holds it" : "");
      continue;  // another interface of the same device may open
    }
    g_last_open_error = 0;
    return std::make_unique<HidLink>(w, r, std::move(wslot), std::move(rslot));
  }
  return nullptr;
}

std::vector<HidDeviceInfo> hid_candidates(const UsbMatch& usb) {
  std::vector<HidDeviceInfo> out;
  for (auto& d : hid_enumerate(usb.vid, usb.pid))
    if ((!usb.usage_page || d.usage_page == *usb.usage_page) && (usb.output_report == 0 || d.out_len == usb.output_report))
      out.push_back(std::move(d));
  return out;
}

bool hid_present(const UsbMatch& usb) { return !hid_candidates(usb).empty(); }

bool matches_usb_path(std::wstring_view path, const UsbMatch& usb) {
  const std::wstring needle = std::format(L"vid_{:04x}&pid_{:04x}", usb.vid, usb.pid);
  if (path.size() < needle.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= path.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size() && ok; ++j) {
      wchar_t c = path[i + j];
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
      ok = c == needle[j];
    }
    if (ok) return true;
  }
  return false;
}

std::unique_ptr<DeviceLink> open_hid(const UsbMatch& usb) {
  if (g_leaked_slots.load() >= kMaxLeakedSlots) return nullptr;
  return open_hid_candidates(hid_candidates(usb));
}

}  // namespace ld
