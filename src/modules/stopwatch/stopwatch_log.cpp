// src/modules/stopwatch/stopwatch_log.cpp
#include "modules/stopwatch/stopwatch_log.h"
#include <windows.h>
#include <cstdlib>
#include <format>
#include <vector>

namespace ld {

std::string format_hms(long long s) {
  bool neg = s < 0;
  long long a = std::llabs(s);
  return std::format("{}{:02}:{:02}:{:02}", neg ? "-" : "", a / 3600, (a % 3600) / 60, a % 60);
}

std::optional<long long> parse_hms(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  bool neg = !s.empty() && s.front() == '-';
  if (neg) s.remove_prefix(1);
  long long parts[3] = {0, 0, 0};
  int digits[3] = {0, 0, 0};
  int idx = 0;
  bool digit = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      if (++digits[idx] > 9) return std::nullopt;  // a hand-edited run of digits must not overflow
      parts[idx] = parts[idx] * 10 + (c - '0');
      digit = true;
    } else if (c == ':' && idx < 2) {
      ++idx;
    } else {
      return std::nullopt;
    }
  }
  if (!digit) return std::nullopt;
  long long total = parts[0] * 3600 + parts[1] * 60 + parts[2];
  return neg ? -total : total;
}

std::string format_duration(long long s) {
  if (s < 0) s = 0;
  return std::format("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60);
}

std::string format_csv_row(const CsvRow& r) {
  return std::format("{:04}-{:02}-{:02};{:02}:{:02}:{:02};{:02}:{:02}:{:02};{};{};{}", r.start.tm_year + 1900,
                     r.start.tm_mon + 1, r.start.tm_mday, r.start.tm_hour, r.start.tm_min, r.start.tm_sec,
                     r.end.tm_hour, r.end.tm_min, r.end.tm_sec, format_duration(r.active_s),
                     format_duration(r.paused_s), r.pauses < 0 ? 0 : r.pauses);
}

namespace {
constexpr std::string_view kBom = "\xEF\xBB\xBF";

// Strictly "H:MM:SS" (hours of any width), as written by format_duration or retyped by Excel.
std::optional<long long> parse_duration(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\r')) s.remove_suffix(1);
  const auto c1 = s.find(':');
  const auto c2 = c1 == std::string_view::npos ? c1 : s.find(':', c1 + 1);
  if (c2 == std::string_view::npos || s.find(':', c2 + 1) != std::string_view::npos) return std::nullopt;
  if (c1 == 0 || c2 - c1 != 3 || s.size() - c2 != 3) return std::nullopt;  // two-digit minutes and seconds
  if (s[c1 + 1] > '5' || s[c2 + 1] > '5') return std::nullopt;               // at most 59
  if (s.front() == '-') return std::nullopt;
  return parse_hms(s);  // digits only: spaces inside or letters are rejected
}
}  // namespace

long long sum_csv_active(std::string_view content) {
  if (content.starts_with(kBom)) content.remove_prefix(kBom.size());
  long long total = 0;
  while (!content.empty()) {
    const auto nl = content.find('\n');
    std::string_view line = content.substr(0, nl);
    content.remove_prefix(nl == std::string_view::npos ? content.size() : nl + 1);
    // date;start;end;active;...: the fourth field
    std::size_t at = 0;
    for (int i = 0; i < 3 && at != std::string_view::npos; ++i) {
      at = line.find(';', at);
      if (at != std::string_view::npos) ++at;
    }
    if (at == std::string_view::npos) continue;
    const auto stop = line.find(';', at);
    if (auto v = parse_duration(line.substr(at, stop == std::string_view::npos ? line.npos : stop - at))) total += *v;
  }
  return total;
}

unsigned long append_csv_row(const std::filesystem::path& file, const CsvRow& row) {
  // FILE_APPEND_DATA without FILE_WRITE_DATA: every write lands at the end, nothing can be overwritten.
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError();
  std::string out;
  LARGE_INTEGER size{};
  DWORD err = GetFileSizeEx(h, &size) ? 0 : GetLastError();
  if (!err && size.QuadPart == 0) {
    out += kBom;
    out += kCsvHeader;
    out += "\r\n";
  } else if (!err) {  // a hand-edited file may lack its final newline: never glue the row to its last line
    LARGE_INTEGER last{};
    last.QuadPart = size.QuadPart - 1;
    char c = 0;
    DWORD got = 0;
    if (!SetFilePointerEx(h, last, nullptr, FILE_BEGIN) || !ReadFile(h, &c, 1, &got, nullptr) || got != 1)
      err = GetLastError() ? GetLastError() : ERROR_READ_FAULT;
    else if (c != '\n')
      out += "\r\n";
  }
  if (!err) {
    out += format_csv_row(row);
    out += "\r\n";
    DWORD wrote = 0;
    if (!WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr))
      err = GetLastError();
    else if (wrote != out.size())
      err = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(h))
      err = GetLastError();
  }
  CloseHandle(h);
  return err;
}

}  // namespace ld
