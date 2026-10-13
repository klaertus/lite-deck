#include "web/assets.h"
#include <windows.h>
#include <array>
#include <string>

namespace ld {

namespace {

struct Entry {
  std::string_view path;
  const wchar_t* resource;
  std::string_view type;
};
constexpr Entry kEntries[] = {
    {"/", L"INDEX_HTML", "text/html; charset=utf-8"},
    {"/app.css", L"APP_CSS", "text/css; charset=utf-8"},
    {"/app.js", L"APP_JS", "text/javascript; charset=utf-8"},
    {"/appearance.js", L"APPEARANCE_JS", "text/javascript; charset=utf-8"},
    {"/elements.js", L"ELEMENTS_JS", "text/javascript; charset=utf-8"},
    {"/settings.js", L"SETTINGS_JS", "text/javascript; charset=utf-8"},
    {"/deck.js", L"DECK_JS", "text/javascript; charset=utf-8"},
};
constexpr std::size_t kCount = std::size(kEntries);

std::string_view resource(const Entry& e) {
  HRSRC res = FindResourceW(nullptr, e.resource, RT_RCDATA);  // nullptr: the executable itself
  if (!res) return {};
  HGLOBAL handle = LoadResource(nullptr, res);
  const DWORD size = SizeofResource(nullptr, res);
  const void* data = handle ? LockResource(handle) : nullptr;
  if (!data || size == 0) return {};
  return std::string_view(static_cast<const char*>(data), size);
}

// Every file and its ETag, found and hashed once, at the first request (the resources do not change
// while the executable runs).
struct Files {
  std::array<std::string_view, kCount> data;
  std::array<std::string, kCount> etag;
  Files() {
    for (std::size_t i = 0; i < kCount; ++i) {
      data[i] = resource(kEntries[i]);
      if (!data[i].empty()) etag[i] = asset_etag(data[i]);
    }
  }
};

}  // namespace

std::optional<Asset> embedded_asset(std::string_view path) {
  static const Files files;  // thread-safe initialisation: web threads may ask at once
  for (std::size_t i = 0; i < kCount; ++i) {
    if (kEntries[i].path != path) continue;
    if (files.data[i].empty()) return std::nullopt;
    return Asset{files.data[i], kEntries[i].type, files.etag[i]};
  }
  return std::nullopt;
}

}  // namespace ld
