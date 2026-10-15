// src/modules/stopwatch/stopwatch_log.h
#pragma once
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ld {
std::string format_hms(long long seconds);                // "[-]HH:MM:SS", the key's display
std::optional<long long> parse_hms(std::string_view s);   // "[-]H:M:S", spaces allowed

// The CSV log: ';' separated (Excel FR/BE), UTF-8 with a BOM, one line per save under this header.
inline constexpr std::string_view kCsvHeader = "date;start;end;active;paused;pauses";

struct CsvRow {
  std::tm start{};         // local time of the session's first start; `date` is its day, even past midnight
  std::tm end{};           // local time of the save
  long long active_s = 0;  // counted time
  long long paused_s = 0;  // time paused between the start and the save
  long long pauses = 0;
};

std::string format_duration(long long seconds);  // "H:MM:SS", hours unbounded (26:03:10), negative as 0
std::string format_csv_row(const CsvRow& row);    // "2026-10-15;09:00:00;17:30:00;7:45:00;0:45:00;2", no newline
long long sum_csv_active(std::string_view content);  // sum of the `active` column; header, BOM and bad lines skipped

// Appends `row` to the CSV at `file`, creating it (BOM and header) if missing or empty. Never truncates,
// never creates the folder. Returns 0 or the Windows error (ERROR_PATH_NOT_FOUND, ERROR_SHARING_VIOLATION...).
unsigned long append_csv_row(const std::filesystem::path& file, const CsvRow& row);
}  // namespace ld
