#pragma once
#include <filesystem>
#include <string>
namespace ld {

inline constexpr wchar_t kAppDirName[] = L"lite-deck";
inline constexpr wchar_t kLegacyAppDirName[] = L"libre-deck";  // the project's former name

std::filesystem::path roaming_app_data();  // %APPDATA% ("" if unknown)
// <base>\lite-deck (base: %APPDATA%), created if missing.
std::filesystem::path app_data_dir(const std::filesystem::path& base);

// What migrate_legacy_data_dir did, and a sentence for the log.
struct DataDirMigration {
  enum class Kind { None, Moved, Copied, Failed } kind = Kind::None;
  std::string message;
};

// Data of the former name: when <base>\lite-deck is missing (or an empty folder) and
// <base>\libre-deck exists, renames the old folder to the new name. If the rename fails (a file is
// locked), copies config.json, state.json and logs\ into <base>\lite-deck and leaves the old folder.
DataDirMigration migrate_legacy_data_dir(const std::filesystem::path& base);

}  // namespace ld
