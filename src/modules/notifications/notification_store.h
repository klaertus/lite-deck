#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "core/types.h"

namespace ld {

struct NotificationItem {
  std::uint32_t id = 0;
  std::wstring app, title, body;
  std::uint64_t seq = 0;  // first-seen order
  TimePoint created{};    // when Windows received it, on the loop clock
};

struct IncomingNotification {
  std::uint32_t id = 0;
  std::wstring app, title, body;
  Duration age{};  // time since Windows received it, measured when fetched
};

// Mirror of the Windows notification list (loop thread only). An acknowledged id is remembered until
// Windows stops listing it, so a removal that Windows has not processed yet does not resurrect it.
class NotificationStore {
public:
  // `now` (loop clock) turns each incoming age into a creation time.
  std::vector<NotificationItem> sync(const std::vector<IncomingNotification>& incoming, TimePoint now);
  bool remove(std::uint32_t id);
  std::optional<NotificationItem> current() const;
  std::size_t count() const { return items_.size(); }
  bool contains(std::uint32_t id) const { return items_.count(id) != 0; }

private:
  std::map<std::uint32_t, NotificationItem> items_;
  std::set<std::uint32_t> acknowledged_;
  std::uint64_t next_seq_ = 0;
};

}  // namespace ld
