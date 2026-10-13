#include "core/log.h"
#include <windows.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include "core/strings.h"

namespace ld {
namespace {
std::mutex g_mu;
std::filesystem::path g_file;
std::atomic<int> g_min{static_cast<int>(LogLevel::Info)};
std::uintmax_t g_max = 1 << 20;
int g_keep = 3;
constexpr ULONGLONG kRotateRetryMs = 60'000;
ULONGLONG g_retry_at = 0;  // GetTickCount64() before which a failed rotation is not tried again

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    default: return "ERROR";
  }
}

std::filesystem::path numbered(int i) { return std::filesystem::path(g_file.wstring() + L"." + std::to_wstring(i)); }

// Makes `staged` backup 1: drops the oldest backup and shifts the others up.
void shift_into_backups(const std::filesystem::path& staged) {
  std::error_code ec;
  std::filesystem::remove(numbered(g_keep), ec);
  for (int i = g_keep - 1; i >= 1; --i)
    if (std::filesystem::exists(numbered(i), ec)) std::filesystem::rename(numbered(i), numbered(i + 1), ec);
  std::filesystem::rename(staged, numbered(1), ec);
}

bool truncate_live() {
  HANDLE h = CreateFileW(g_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

// The backups are touched only once the live file's content is safely set aside, so a failed
// rotation never costs them. True if the live file is now empty or gone.
bool rotate() {
  const ULONGLONG now = GetTickCount64();
  if (now < g_retry_at) return false;
  std::error_code ec;
  const std::filesystem::path staged = g_file.wstring() + L".rotating";
  std::filesystem::remove(staged, ec);  // left by an interrupted rotation
  if (MoveFileExW(g_file.c_str(), staged.c_str(), 0)) {
    shift_into_backups(staged);
    return true;
  }
  // The rename fails while another program holds the file without delete sharing (a tail with
  // Get-Content -Wait, a viewer): copy the content aside, then empty the file in place.
  if (CopyFileW(g_file.c_str(), staged.c_str(), FALSE)) {
    if (truncate_live()) {
      shift_into_backups(staged);
      return true;
    }
    std::filesystem::remove(staged, ec);
  }
  g_retry_at = now + kRotateRetryMs;
  OutputDebugStringW((L"lite-deck: cannot rotate " + g_file.wstring() + L"; retrying in a minute\n").c_str());
  return false;
}
}  // namespace

void log_init(std::filesystem::path file, LogLevel min_level, std::uintmax_t max_bytes, int keep) {
  std::lock_guard lock(g_mu);
  g_file = std::move(file);
  g_min.store(static_cast<int>(min_level));
  g_max = max_bytes;
  g_keep = keep;
  g_retry_at = 0;
  if (!g_file.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(g_file.parent_path(), ec);
  }
}

std::string format_log_time(const SYSTEMTIME& st) {
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds);
}

void log_write(LogLevel level, std::string_view message) {
  if (static_cast<int>(level) < g_min.load()) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::string line = std::format("{} {} {}\n", format_log_time(st), level_name(level), message);
  std::lock_guard lock(g_mu);
  OutputDebugStringW(widen(line).c_str());
  if (g_file.empty()) return;
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(g_file, ec);
  if (!ec && size + line.size() > g_max && !rotate() && size + line.size() > 2 * g_max)
    return;  // cannot rotate: the file stops growing at twice the limit until a retry succeeds
  std::ofstream f(g_file, std::ios::app | std::ios::binary);
  f << line;
}

}  // namespace ld
