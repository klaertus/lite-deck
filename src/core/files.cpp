// src/core/files.cpp
#include "core/files.h"
#include <windows.h>

namespace ld {

bool write_file_atomic(const std::filesystem::path& file, std::string_view content) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::filesystem::path tmp = file;
  tmp += ".tmp";
  // CREATE_ALWAYS also replaces a .tmp left behind by a crash during a previous save.
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) != 0 &&
            written == content.size() && FlushFileBuffers(h) != 0;
  DWORD err = ok ? 0 : GetLastError();
  CloseHandle(h);
  if (ok && !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ok = false;
    err = GetLastError();
  }
  if (!ok) {
    std::filesystem::remove(tmp, ec);
    SetLastError(err);
  }
  return ok;
}

nlohmann::json parse_json_bounded(std::string_view text, bool ignore_comments, bool* too_deep) {
  using nlohmann::json;
  bool deep = false;
  json doc = json::parse(
      text,
      [&deep](int depth, json::parse_event_t event, json&) {
        if ((event == json::parse_event_t::object_start || event == json::parse_event_t::array_start) &&
            depth >= kMaxFileJsonDepth) {
          deep = true;
          return false;  // the nested value is not even built
        }
        return true;
      },
      false, ignore_comments);
  if (too_deep) *too_deep = deep;
  if (deep) return json(json::value_t::discarded);
  return doc;
}

}  // namespace ld
