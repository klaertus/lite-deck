// src/modules/stopwatch/stopwatch_log.h
#pragma once
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace ld {
// Same file format as the StreamDock "Stopwatch Save" plugin.
std::string format_hms(long long seconds);                // "[-]HH:MM:SS"
std::optional<long long> parse_hms(std::string_view s);   // "[-]H:M:S", spaces allowed
std::string format_entry(const std::tm& when, long long elapsed_seconds);
long long sum_entries(std::string_view content);           // sum of "| HH:MM:SS" on lines containing '|'
std::string append_entry(std::string_view content, std::string_view entry_line);
}  // namespace ld
