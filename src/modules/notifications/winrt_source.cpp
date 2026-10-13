// src/modules/notifications/winrt_source.cpp
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>
#include <map>
#include <memory>
#include <algorithm>
#include <mutex>
#include <set>
#include "core/log.h"
#include "core/winrt.h"
#include "modules/notifications/notifications.h"

namespace ld {
namespace {
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::System::Threading::ThreadPool;

IncomingNotification convert(const UserNotification& n) {
  IncomingNotification out;
  out.id = n.Id();
  try {
    out.app = to_wide(n.AppInfo().DisplayInfo().DisplayName());
  } catch (...) {
  }
  try {
    auto binding = n.Notification().Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
    if (binding) {
      auto texts = binding.GetTextElements();
      if (texts.Size() > 0) out.title = to_wide(texts.GetAt(0).Text());
      std::wstring body;
      for (uint32_t i = 1; i < texts.Size(); ++i) {
        if (!body.empty()) body += L" ";
        body += to_wide(texts.GetAt(i).Text());
      }
      out.body = body;
    }
  } catch (...) {
  }
  return out;
}

// Converted toasts by id; touched only from fetch Completed handlers, guarded because they may overlap.
struct CachedToast {
  IncomingNotification n;
  winrt::clock::time_point created{};
};
struct ToastCache {
  std::mutex mu;
  std::map<std::uint32_t, CachedToast> items;
};

winrt::clock::time_point creation_time(const UserNotification& n) {
  try {
    return n.CreationTime();
  } catch (...) {
    return winrt::clock::now();
  }
}

class WinrtSource final : public NotificationSource {
public:
  void request_access(std::function<void(NotifAccess)> done) override {
    try {
      listener_ = UserNotificationListener::Current();
      listener_.RequestAccessAsync().Completed([done](auto const& op, AsyncStatus status) {
        if (status != AsyncStatus::Completed) return done(NotifAccess::Unavailable);
        try {
          done(op.GetResults() == UserNotificationListenerAccessStatus::Allowed ? NotifAccess::Allowed
                                                                              : NotifAccess::Denied);
        } catch (...) {
          done(NotifAccess::Unavailable);
        }
      });
    } catch (...) {
      log_warn("notifications: UserNotificationListener unavailable");
      done(NotifAccess::Unavailable);
    }
  }
  void check_access(std::function<void(NotifAccess)> done) override {
    try {
      if (!listener_) listener_ = UserNotificationListener::Current();
      ThreadPool::RunAsync([listener = listener_, done](IAsyncAction const&) {
        NotifAccess a = NotifAccess::Unavailable;
        try {
          switch (listener.GetAccessStatus()) {
            case UserNotificationListenerAccessStatus::Allowed: a = NotifAccess::Allowed; break;
            case UserNotificationListenerAccessStatus::Denied: a = NotifAccess::Denied; break;
            default: break;  // Unspecified: never answered
          }
        } catch (...) {
        }
        done(a);
      });
    } catch (...) {
      done(NotifAccess::Unavailable);
    }
  }
  void fetch(std::function<void(std::optional<std::vector<IncomingNotification>>)> done) override {
    try {
      listener_.GetNotificationsAsync(NotificationKinds::Toast).Completed(
          [done, cache = cache_](auto const& op, AsyncStatus status) {
            if (status != AsyncStatus::Completed) return done(std::nullopt);
            try {
              std::vector<IncomingNotification> out;
              {
                std::lock_guard lock(cache->mu);
                std::set<std::uint32_t> live;
                const auto now = winrt::clock::now();
                for (auto const& n : op.GetResults()) {
                  std::uint32_t id = n.Id();
                  live.insert(id);
                  auto it = cache->items.find(id);
                  if (it == cache->items.end()) it = cache->items.emplace(id, CachedToast{convert(n), creation_time(n)}).first;
                  IncomingNotification item = it->second.n;
                  item.age = std::max(Duration{0}, std::chrono::duration_cast<Duration>(now - it->second.created));
                  out.push_back(std::move(item));
                }
                std::erase_if(cache->items, [&](auto const& kv) { return !live.contains(kv.first); });
              }
              done(std::move(out));
            } catch (...) {
              done(std::nullopt);
            }
          });
    } catch (...) {
      done(std::nullopt);
    }
  }
  // RemoveNotification is a synchronous call into the notification platform: run it on the thread pool,
  // never on the loop thread.
  void remove(std::uint32_t id) override {
    try {
      ThreadPool::RunAsync([listener = listener_, id](IAsyncAction const&) {
        try {
          listener.RemoveNotification(id);
        } catch (...) {
          log_warn("notifications: cannot remove {}", id);
        }
      });
    } catch (...) {
      log_warn("notifications: cannot remove {}", id);
    }
  }

private:
  UserNotificationListener listener_{nullptr};
  std::shared_ptr<ToastCache> cache_ = std::make_shared<ToastCache>();
};

}  // namespace

std::unique_ptr<NotificationSource> make_winrt_notification_source() { return std::make_unique<WinrtSource>(); }

}  // namespace ld
