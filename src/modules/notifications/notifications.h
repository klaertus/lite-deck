// src/modules/notifications/notifications.h
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "modules/module.h"
#include "modules/notifications/notification_store.h"

namespace ld {
enum class NotifAccess { Allowed, Denied, Unavailable };
// Polled: UserNotificationListener::NotificationChanged only fires for packaged apps (or apps with a package
// identity), and the daemon is a plain Win32 program. GetNotificationsAsync once a second is cheap.
inline constexpr Duration kNotificationPoll{1000};
// While access is not granted, how often it is checked again (the user may grant it in Settings).
inline constexpr Duration kAccessRecheck{30000};
// While access is granted, the least time between two checks caused by failed fetches (revoked access).
inline constexpr Duration kFailedFetchRecheck{5000};
// Callbacks may run on any thread (WinRT).
class NotificationSource {
public:
  virtual ~NotificationSource() = default;
  // Asks once, at start.
  virtual void request_access(std::function<void(NotifAccess)> done) = 0;
  // The current access status, without asking the user (cheap; answers off the loop thread).
  virtual void check_access(std::function<void(NotifAccess)> done) = 0;
  virtual void fetch(std::function<void(std::optional<std::vector<IncomingNotification>>)> done) = 0;
  virtual void remove(std::uint32_t id) = 0;
};
std::unique_ptr<NotificationSource> make_winrt_notification_source();
std::unique_ptr<Module> make_notifications_module(std::function<std::unique_ptr<NotificationSource>()> factory);
ModuleInfo notifications_module_info();
}  // namespace ld
