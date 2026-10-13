// src/core/log.h
#pragma once
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <windows.h>

namespace ld {

enum class LogLevel { Debug, Info, Warn, Error };

// Empty path = OutputDebugString only. Thread-safe.
void log_init(std::filesystem::path file, LogLevel min_level, std::uintmax_t max_bytes = 1 << 20, int keep = 3);
std::string format_log_time(const SYSTEMTIME& st);  // "YYYY-MM-DD HH:MM:SS.mmm"
void log_write(LogLevel level, std::string_view message);

template <class... A> void log_debug(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Debug, std::format(f, std::forward<A>(a)...)); }
template <class... A> void log_info(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A> void log_warn(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Warn, std::format(f, std::forward<A>(a)...)); }
template <class... A> void log_error(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Error, std::format(f, std::forward<A>(a)...)); }

}  // namespace ld
