// src/modules/stopwatch/stopwatch_log.cpp
#include "modules/stopwatch/stopwatch_log.h"
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

std::string format_entry(const std::tm& t, long long elapsed) {
  return std::format("{} - {:02} - {:02} - {:02}:{:02}:{:02} | {}", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec, format_hms(elapsed));
}

namespace {
std::vector<std::string> split_lines(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
    if (s[i] == '\n') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(s[i]);
    }
  }
  out.push_back(cur);  // like JavaScript split: a trailing newline yields a final ""
  return out;
}
}  // namespace

long long sum_entries(std::string_view content) {
  long long total = 0;
  for (const auto& line : split_lines(content)) {
    auto bar = line.find('|');
    if (bar == std::string::npos) continue;
    if (auto v = parse_hms(std::string_view(line).substr(bar + 1))) total += *v;
  }
  return total;
}

std::string append_entry(std::string_view content, std::string_view entry) {
  std::string c(content);
  if (!c.empty() && c.back() != '\n') c += '\n';
  c += entry;
  c += '\n';
  std::vector<std::string> body;
  for (auto& l : split_lines(c))
    if (!l.starts_with("Total :")) body.push_back(std::move(l));
  body.push_back("Total : " + format_hms(sum_entries(c)));
  std::string out;
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (i) out += '\n';
    out += body[i];
  }
  return out + '\n';
}

}  // namespace ld
