// src/device/device_manager.cpp
#include "device/device_manager.h"
#include "core/log.h"

namespace ld {

DeviceManager::DeviceManager(LoopApi& loop, const DeviceEntry& entry, Opener opener, std::function<bool()> other_app_running)
    : loop_(loop), entry_(entry), opener_(std::move(opener)), other_app_running_(std::move(other_app_running)) {}

DeviceManager::~DeviceManager() {
  stopped_ = true;
  on_state = nullptr;
  on_input = nullptr;
  on_connected = nullptr;
  loop_.cancel(retry_timer_);
  disconnect();
}

void DeviceManager::set_state(State s) {
  if (state_ == s) return;
  state_ = s;
  if (on_state) on_state(s);
}

void DeviceManager::start() { try_connect(); }

void DeviceManager::schedule_retry() {
  if (stopped_ || retry_timer_) return;
  Duration delay = kIdleRetry;
  if (state_ == State::Busy) {
    delay = kRetry;
  } else if (fast_retries_ > 0) {
    --fast_retries_;
    delay = kRetry;
  }
  retry_timer_ = loop_.at(loop_.now() + delay, [this] {
    retry_timer_ = 0;
    try_connect();
  });
}

void DeviceManager::device_arrived() {
  if (stopped_ || link_ || state_ == State::Busy) return;
  fast_retries_ = kFastRetries;
  loop_.cancel(retry_timer_);
  retry_timer_ = loop_.at(loop_.now() + kArrivalDelay, [this] {
    retry_timer_ = 0;
    try_connect();
  });
}

void DeviceManager::try_connect() {
  if (stopped_ || link_) return;
  if (other_app_running_ && other_app_running_()) {
    set_state(State::Busy);
    schedule_retry();
    return;
  }
  link_ = opener_();
  if (!link_) {
    set_state(State::Disconnected);
    schedule_retry();
    return;
  }
  dev_ = entry_.factory(*link_);
  if (!dev_) {
    log_error("{}: the driver did not start", entry_.layout.name);
    disconnect();
    schedule_retry();
    return;
  }
  dev_->initialize(brightness_);
  apply_rings();
  if (check_failed()) return;
  heartbeat_timer_ = loop_.every(entry_.layout.heartbeat, [this] {
    dev_->heartbeat();
    check_failed();
  });
  loop_.add_handle(link_->read_event(), [this] { on_read(); });
  if (!link_->start_read()) {
    disconnect();
    schedule_retry();
    return;
  }
  if (other_app_running_) {
    busy_timer_ = loop_.every(kRetry, [this] {
      if (!other_app_running_()) return;
      log_info("another app took the device; releasing it");
      disconnect(State::Busy);
      schedule_retry();
    });
  }
  log_info("{} connected", entry_.layout.name);
  set_state(State::Connected);
  if (!dev_) return;
  if (asleep_) {
    dev_->sleep();
    check_failed();
  } else if (on_connected) {
    on_connected();
  }
}

void DeviceManager::disconnect(State next) {
  if (link_) loop_.remove_handle(link_->read_event());
  loop_.cancel(heartbeat_timer_);
  loop_.cancel(flush_timer_);
  loop_.cancel(busy_timer_);
  heartbeat_timer_ = flush_timer_ = busy_timer_ = 0;
  dev_.reset();
  if (link_) {
    log_warn("{} disconnected", entry_.layout.name);
    // A link that was fully connected and dropped is usually a re-enumeration: retry fast for a while.
    // A link that opened but failed initialisation does not re-arm (it would hammer a broken device).
    if (state_ == State::Connected) fast_retries_ = kFastRetries;
  }
  link_.reset();
  set_state(next);
}

bool DeviceManager::check_failed() {
  if (!dev_ || !dev_->failed()) return false;
  disconnect();
  schedule_retry();
  return true;
}

void DeviceManager::on_read() {
  if (!link_) return;
  auto report = link_->finish_read();
  if (!report) {
    disconnect();
    schedule_retry();
    return;
  }
  const DeviceInput in = dev_ ? dev_->decode_input(*report) : DeviceInput{};
  if (in.kind != InputKind::None && on_input) on_input(in);
  if (link_ && !link_->start_read()) {
    disconnect();
    schedule_retry();
  }
}

void DeviceManager::schedule_flush() {
  if (!dev_ || flush_timer_) return;
  auto when = dev_->next_flush(loop_.now());
  if (!when) return;
  flush_timer_ = loop_.at(*when, [this] {
    flush_timer_ = 0;
    if (!dev_) return;
    dev_->flush(loop_.now());
    if (!check_failed()) schedule_flush();
  });
}

void DeviceManager::key_image(int slot, const Image& image) {
  if (!dev_ || asleep_) return;
  dev_->key_image(slot, image);
  schedule_flush();
}

void DeviceManager::strip_image(const Image& image) {
  if (!dev_ || asleep_) return;
  dev_->strip_image(image);
  schedule_flush();
}

void DeviceManager::apply_rings() {
  if (dev_ && rings_ && !asleep_ && entry_.layout.knob_rings) dev_->set_rings(rings_->first, rings_->second);
}

void DeviceManager::rings(std::span<const Rgb> colors, std::uint8_t brightness) {
  if (!rings_) rings_.emplace();
  rings_->first.assign(colors.begin(), colors.end());  // reuses its capacity
  rings_->second = brightness;
  apply_rings();
  check_failed();
}

void DeviceManager::set_brightness(std::uint8_t pct) {
  brightness_ = pct;
  if (dev_ && !asleep_) dev_->set_brightness(pct);
  check_failed();
}

void DeviceManager::sleep() {
  asleep_ = true;
  loop_.cancel(flush_timer_);
  flush_timer_ = 0;
  if (dev_) dev_->sleep();
  check_failed();
}

void DeviceManager::wake() {
  asleep_ = false;
  if (!dev_) return;
  dev_->initialize(brightness_);
  apply_rings();
  if (check_failed()) return;
  if (on_connected) on_connected();
}

void DeviceManager::shutdown() {
  stopped_ = true;
  loop_.cancel(retry_timer_);
  retry_timer_ = 0;
  if (dev_) dev_->shutdown();
  disconnect();
}

}  // namespace ld
