// src/web/event_hub.cpp
#include "web/event_hub.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <set>

namespace ld {
namespace detail {

struct HubClient {
  int id = 0;
  std::map<std::string, std::string> events;  // name -> latest data
  std::set<int> frames;                       // targets with a new frame since the last send
  TimePoint last_frame{};
};

struct HubCore {
  std::mutex mu;
  std::condition_variable cv;
  std::list<HubClient> clients;
  std::atomic<int> count{0};
  bool closed = false;
  int next_id = 1;
  std::function<void()> on_idle;

  HubClient* find(int id) {
    for (auto& c : clients)
      if (c.id == id) return &c;
    return nullptr;
  }
};

}  // namespace detail

EventHub::EventHub() : core_(std::make_shared<detail::HubCore>()) {}

std::unique_ptr<EventHub::Stream> EventHub::subscribe() {
  std::lock_guard lock(core_->mu);
  if (core_->closed || static_cast<int>(core_->clients.size()) >= kMaxStreams) return nullptr;
  const int id = core_->next_id++;
  core_->clients.push_back(detail::HubClient{id, {}, {}, {}});
  core_->count.store(static_cast<int>(core_->clients.size()));
  return std::unique_ptr<Stream>(new Stream(core_, id));
}

EventHub::Stream::~Stream() {
  std::function<void()> idle;
  {
    std::lock_guard lock(core_->mu);
    core_->clients.remove_if([&](const detail::HubClient& c) { return c.id == id_; });
    core_->count.store(static_cast<int>(core_->clients.size()));
    if (core_->clients.empty()) idle = core_->on_idle;
  }
  if (idle) idle();  // outside the lock: it may wait on the loop
}

void EventHub::publish(const std::string& event, const nlohmann::json& data) {
  if (core_->count.load() == 0) return;
  const std::string text = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  {
    std::lock_guard lock(core_->mu);
    for (auto& c : core_->clients) c.events[event] = text;
  }
  core_->cv.notify_all();
}

void EventHub::frame(int target) {
  if (core_->count.load() == 0) return;
  bool wake = false;
  {
    std::lock_guard lock(core_->mu);
    for (auto& c : core_->clients) {
      wake = wake || c.frames.empty();  // a stream already holding frames has its wake-up planned
      c.frames.insert(target);
    }
  }
  if (wake) core_->cv.notify_all();
}

void EventHub::close() {
  {
    std::lock_guard lock(core_->mu);
    core_->closed = true;
  }
  core_->cv.notify_all();
}

int EventHub::streams() const { return core_->count.load(); }

void EventHub::on_idle(std::function<void()> fn) {
  std::lock_guard lock(core_->mu);
  core_->on_idle = std::move(fn);
}

std::string EventHub::Stream::next(Duration ping) {
  std::unique_lock lock(core_->mu);
  const TimePoint deadline = Clock::now() + ping;
  for (;;) {
    if (core_->closed) return {};
    detail::HubClient* c = core_->find(id_);
    if (!c) return {};
    const TimePoint now = Clock::now();
    const bool frames_due = !c->frames.empty() && now >= c->last_frame + kFrameInterval;
    if (!c->events.empty() || frames_due) {
      std::string out;
      for (const auto& [name, data] : c->events) out += "event: " + name + "\ndata: " + data + "\n\n";
      c->events.clear();
      if (frames_due) {
        nlohmann::json keys = nlohmann::json::array();
        bool strip = false;
        for (int t : c->frames) {
          if (t < 0) strip = true;
          else keys.push_back(t);
        }
        out += "event: frame\ndata: " + nlohmann::json{{"keys", keys}, {"strip", strip}}.dump() + "\n\n";
        c->frames.clear();
        c->last_frame = now;
      }
      return out;
    }
    if (now >= deadline) return ": ping\n\n";
    TimePoint wake = deadline;
    if (!c->frames.empty()) wake = std::min(wake, c->last_frame + kFrameInterval);
    core_->cv.wait_until(lock, wake);
  }
}

}  // namespace ld
