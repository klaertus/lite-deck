// src/device/device_manager.h
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "core/loop_api.h"
#include "device/device.h"
#include "device/hid.h"
#include "layout/deck_sink.h"

namespace ld {

// Owns the connection lifecycle of the active device: connect/retry, vendor app detection
// (`other_app_running`, may be empty), heartbeat (layout.heartbeat), reads, paced flushes. The driver
// comes from `entry`, which must outlive the manager.
class DeviceManager final : public DeckSink {
public:
  enum class State { Disconnected, Busy, Connected };
  using Opener = std::function<std::unique_ptr<DeviceLink>()>;
  static constexpr Duration kRetry{2000};
  static constexpr Duration kIdleRetry{30000};   // device absent: safety-net poll; arrivals come from WM_DEVICECHANGE
  static constexpr Duration kArrivalDelay{500};  // let the new HID interfaces settle
  static constexpr int kFastRetries = 5;         // 2 s retries after an arrival or a lost link (re-enumeration)

  DeviceManager(LoopApi& loop, const DeviceEntry& entry, Opener opener, std::function<bool()> other_app_running);
  ~DeviceManager() override;

  void start();
  void set_brightness(std::uint8_t pct);
  void sleep();
  void wake();
  void shutdown();
  void device_arrived();  // a HID interface of the active device appeared (WM_DEVICECHANGE / DBT_DEVICEARRIVAL)
  State state() const { return state_; }

  std::function<void(const DeviceInput&)> on_input;
  std::function<void()> on_connected;  // after (re)initialization: re-render everything
  std::function<void(State)> on_state;

  void key_image(int slot, const Image& image) override;
  void strip_image(const Image& image) override;
  void rings(std::span<const Rgb> colors, std::uint8_t brightness) override;

private:
  void try_connect();
  void disconnect(State next = State::Disconnected);
  void schedule_retry();
  void schedule_flush();
  void on_read();
  bool check_failed();
  void set_state(State s);
  void apply_rings();

  LoopApi& loop_;
  const DeviceEntry& entry_;
  Opener opener_;
  std::function<bool()> other_app_running_;
  std::unique_ptr<DeviceLink> link_;
  std::unique_ptr<Device> dev_;
  State state_ = State::Disconnected;
  std::uint8_t brightness_ = 60;
  std::optional<std::pair<std::vector<Rgb>, std::uint8_t>> rings_;
  TimerId retry_timer_ = 0, heartbeat_timer_ = 0, flush_timer_ = 0, busy_timer_ = 0;
  bool asleep_ = false;
  bool stopped_ = false;
  int fast_retries_ = 0;
};

}  // namespace ld
