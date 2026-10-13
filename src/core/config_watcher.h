// src/core/config_watcher.h
#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include "core/loop_api.h"

namespace ld {

// True if the FILE_NOTIFY_INFORMATION chain in `buffer` (`len` valid bytes) names `file_name`,
// ignoring case. Never reads past `len`.
bool notify_mentions(const void* buffer, std::size_t len, std::wstring_view file_name);

// Watches one file through an overlapped ReadDirectoryChangesW on its folder. The completion event
// is a loop handle: no thread, no polling. `on_change` runs on the loop, once, kDebounce after the
// last change (an editor's save is often several writes). If the watch cannot start or stops
// (folder missing or deleted, error), it is retried every `retry`; once it runs again, `on_change`
// fires once, since edits made meanwhile were not seen.
class ConfigWatcher {
public:
  static constexpr Duration kDebounce{300};
  static constexpr Duration kRetry{30000};
  ConfigWatcher(LoopApi& loop, const std::filesystem::path& file, std::function<void()> on_change,
                Duration retry = kRetry);
  ~ConfigWatcher();
  ConfigWatcher(const ConfigWatcher&) = delete;
  ConfigWatcher& operator=(const ConfigWatcher&) = delete;
  bool active() const { return pending_; }

private:
  bool start();  // opens the folder if needed and arms the read; false (folder closed) on failure
  void close_dir();
  void schedule_retry();
  void changed();
  void on_signaled();

  LoopApi& loop_;
  std::filesystem::path folder_;
  std::wstring name_;
  std::function<void()> on_change_;
  Duration retry_period_;
  HANDLE dir_ = nullptr;
  HANDLE event_ = nullptr;
  OVERLAPPED ov_{};
  alignas(DWORD) std::array<std::byte, 8192> buf_{};
  bool pending_ = false;
  bool failing_ = false;  // a failure was logged: retries stay quiet until the watch runs again
  TimerId debounce_ = 0;
  TimerId retry_ = 0;
};

}  // namespace ld
