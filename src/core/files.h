// src/core/files.h: durable writes and bounded JSON parsing for the daemon's own files.
#pragma once
#include <filesystem>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ld {

// Deeper nesting is refused: later steps (copies, comparisons, dump) recurse on the document.
inline constexpr int kMaxFileJsonDepth = 64;

// Writes <file>.tmp, flushes it to disk, then replaces <file> atomically (MoveFileEx, write-through).
// Creates the parent folder. Removes the temp file on failure (GetLastError() keeps the reason).
bool write_file_atomic(const std::filesystem::path& file, std::string_view content);

// json::parse(text, nullptr, false, ignore_comments), except that a document nested deeper than
// kMaxFileJsonDepth is discarded too; `too_deep`, if given, tells the two cases apart.
nlohmann::json parse_json_bounded(std::string_view text, bool ignore_comments = false, bool* too_deep = nullptr);

}  // namespace ld
