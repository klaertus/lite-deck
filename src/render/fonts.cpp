// src/render/fonts.cpp
#include "render/fonts.h"
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include "core/strings.h"

#pragma comment(lib, "dwrite.lib")

namespace ld {
using Microsoft::WRL::ComPtr;

namespace {

// The process-wide shared factory (the Renderer's own): thread-safe, and its system collection is
// already loaded by the time text is drawn.
ComPtr<IDWriteFontCollection> system_collection() {
  ComPtr<IDWriteFactory> factory;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
    return nullptr;
  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(factory->GetSystemFontCollection(&fonts, FALSE))) return nullptr;
  return fonts;
}

std::string family_name(IDWriteFontFamily* family) {
  ComPtr<IDWriteLocalizedStrings> names;
  if (FAILED(family->GetFamilyNames(&names)) || names->GetCount() == 0) return {};
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
  UINT32 length = 0;
  if (FAILED(names->GetStringLength(index, &length))) return {};
  std::wstring name(length + 1, L'\0');
  if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
  name.resize(length);
  return narrow(name);
}

bool less_ci(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; }
bool same_ci(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

}  // namespace

const std::vector<std::string>& system_font_families() {
  static const std::vector<std::string> families = [] {
    std::vector<std::string> out;
    ComPtr<IDWriteFontCollection> fonts = system_collection();
    if (!fonts) return out;
    const UINT32 n = fonts->GetFontFamilyCount();
    out.reserve(n);
    for (UINT32 i = 0; i < n; ++i) {
      ComPtr<IDWriteFontFamily> family;
      if (FAILED(fonts->GetFontFamily(i, &family))) continue;
      if (std::string name = family_name(family.Get()); !name.empty()) out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end(), less_ci);
    out.erase(std::unique(out.begin(), out.end(), same_ci), out.end());
    out.shrink_to_fit();
    return out;
  }();
  return families;
}

bool font_installed(std::wstring_view family) {
  if (family.empty()) return false;
  ComPtr<IDWriteFontCollection> fonts = system_collection();
  if (!fonts) return false;
  const std::wstring name(family);
  UINT32 index = 0;
  BOOL exists = FALSE;
  return SUCCEEDED(fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists;
}

}  // namespace ld
