// src/render/fonts.h
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace ld {

// Family names of the installed fonts (DirectWrite system collection): the en-us name, else the
// first one; UTF-8, sorted case-insensitively, without duplicates. Built on the first call, from any
// thread, then kept for the life of the process: a font installed later appears after a restart.
const std::vector<std::string>& system_font_families();

// True if `family` names an installed family (any of its localized names, case-insensitive).
bool font_installed(std::wstring_view family);

}  // namespace ld
