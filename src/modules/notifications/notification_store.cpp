#include "modules/notifications/notification_store.h"
#include <algorithm>

namespace ld {

std::vector<NotificationItem> NotificationStore::sync(const std::vector<IncomingNotification>& incoming, TimePoint now) {
  std::set<std::uint32_t> ids;
  for (const auto& n : incoming) ids.insert(n.id);
  std::erase_if(items_, [&](const auto& kv) { return !ids.count(kv.first); });
  std::erase_if(acknowledged_, [&](std::uint32_t id) { return !ids.count(id); });
  std::vector<NotificationItem> added;
  for (const auto& n : incoming) {
    if (items_.count(n.id) || acknowledged_.count(n.id)) continue;
    NotificationItem item{n.id, n.app, n.title, n.body, next_seq_++, now - n.age};
    items_[n.id] = item;
    added.push_back(item);
  }
  return added;
}

bool NotificationStore::remove(std::uint32_t id) {
  if (!items_.erase(id)) return false;
  acknowledged_.insert(id);
  return true;
}

std::optional<NotificationItem> NotificationStore::current() const {
  if (items_.empty()) return std::nullopt;
  auto it = std::max_element(items_.begin(), items_.end(),
                             [](const auto& a, const auto& b) { return a.second.seq < b.second.seq; });
  return it->second;
}

}  // namespace ld
