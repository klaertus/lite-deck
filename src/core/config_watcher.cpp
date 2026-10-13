// src/core/config_watcher.cpp
#include "core/config_watcher.h"
#include <cstddef>
#include "core/log.h"
#include "core/strings.h"

namespace ld {

bool notify_mentions(const void* buffer, std::size_t len, std::wstring_view file_name) {
  const auto* base = static_cast<const std::byte*>(buffer);
  constexpr std::size_t kHeader = offsetof(FILE_NOTIFY_INFORMATION, FileName);
  std::size_t off = 0;
  while (off + kHeader <= len) {
    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + off);
    if (off + kHeader + info->FileNameLength > len) break;
    const int chars = static_cast<int>(info->FileNameLength / sizeof(wchar_t));
    if (CompareStringOrdinal(info->FileName, chars, file_name.data(), static_cast<int>(file_name.size()), TRUE) ==
        CSTR_EQUAL)
      return true;
    if (info->NextEntryOffset == 0) break;
    off += info->NextEntryOffset;
  }
  return false;
}

ConfigWatcher::ConfigWatcher(LoopApi& loop, const std::filesystem::path& file, std::function<void()> on_change,
                             Duration retry)
    : loop_(loop),
      folder_(file.parent_path()),
      name_(file.filename().wstring()),
      on_change_(std::move(on_change)),
      retry_period_(retry) {
  event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event_) {
    log_error("config watcher: CreateEvent failed ({}); config.json edits need a restart", GetLastError());
    return;
  }
  loop_.add_handle(event_, [this] { on_signaled(); });
  if (!start()) schedule_retry();
}

ConfigWatcher::~ConfigWatcher() {
  loop_.cancel(debounce_);
  loop_.cancel(retry_);
  if (event_) loop_.remove_handle(event_);
  close_dir();
  if (event_) CloseHandle(event_);
}

void ConfigWatcher::close_dir() {
  if (!dir_) return;
  if (pending_) {
    CancelIoEx(dir_, &ov_);
    DWORD bytes = 0;
    GetOverlappedResult(dir_, &ov_, &bytes, TRUE);  // the cancelled read completes at once
    pending_ = false;
  }
  CloseHandle(dir_);
  dir_ = nullptr;
}

bool ConfigWatcher::start() {
  if (!dir_) {
    HANDLE h = CreateFileW(folder_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      if (!failing_) log_error("config watcher: cannot open {} ({}); retrying", narrow(folder_.wstring()), err);
      failing_ = true;
      return false;
    }
    dir_ = h;
  }
  ResetEvent(event_);
  ov_ = OVERLAPPED{};
  ov_.hEvent = event_;
  pending_ = ReadDirectoryChangesW(dir_, buf_.data(), static_cast<DWORD>(buf_.size()), FALSE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                   nullptr, &ov_, nullptr) != 0;
  if (!pending_) {
    const DWORD err = GetLastError();
    if (!failing_) log_error("config watcher: ReadDirectoryChangesW failed ({}); retrying", err);
    failing_ = true;
    close_dir();
    return false;
  }
  return true;
}

void ConfigWatcher::schedule_retry() {
  loop_.cancel(retry_);
  retry_ = loop_.at(loop_.now() + retry_period_, [this] {
    retry_ = 0;
    if (!start()) {
      schedule_retry();
      return;
    }
    log_info("config watcher running again");
    failing_ = false;
    changed();  // the file may have been edited while nothing watched it
  });
}

void ConfigWatcher::changed() {
  loop_.cancel(debounce_);
  debounce_ = loop_.at(loop_.now() + kDebounce, [this] {
    debounce_ = 0;
    if (on_change_) on_change_();
  });
}

void ConfigWatcher::on_signaled() {
  if (!dir_) return;
  DWORD bytes = 0;
  const bool ok = GetOverlappedResult(dir_, &ov_, &bytes, FALSE) != 0;
  const DWORD err = ok ? 0 : GetLastError();
  pending_ = false;
  ResetEvent(event_);
  const bool overflow = !ok && err == ERROR_NOTIFY_ENUM_DIR;  // too many changes: reload anyway
  if (!ok && !overflow) {  // folder deleted, handle invalid...: close and retry later, never spin
    log_warn("config watcher stopped ({}); retrying", err);
    failing_ = true;
    close_dir();
    schedule_retry();
    return;
  }
  const bool hit = overflow || bytes == 0 || notify_mentions(buf_.data(), bytes, name_);  // 0 bytes = overflow
  if (!start()) schedule_retry();
  if (hit) changed();
}

}  // namespace ld
