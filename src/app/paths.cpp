#include "app/paths.h"
#include <windows.h>
#include <shlobj.h>
#include <format>
#include "core/strings.h"

namespace ld {

std::filesystem::path roaming_app_data() {
  PWSTR raw = nullptr;
  std::filesystem::path base;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw))) base = raw;
  CoTaskMemFree(raw);
  return base;
}

std::filesystem::path app_data_dir(const std::filesystem::path& base) {
  std::filesystem::path dir = base / kAppDirName;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

DataDirMigration migrate_legacy_data_dir(const std::filesystem::path& base) {
  namespace fs = std::filesystem;
  using Kind = DataDirMigration::Kind;
  const fs::path from = base / kLegacyAppDirName;
  const fs::path to = base / kAppDirName;
  std::error_code ec;
  if (base.empty() || !fs::is_directory(from, ec)) return {};
  if (fs::exists(to, ec) || ec) {
    // An empty folder (made by an earlier start that found nothing to migrate) does not count.
    if (!fs::is_directory(to, ec) || !fs::is_empty(to, ec) || ec) return {};
    fs::remove(to, ec);
    if (ec) return {};
  }

  const std::string from_s = narrow(from.wstring());
  const std::string to_s = narrow(to.wstring());
  fs::rename(from, to, ec);
  if (!ec) return {Kind::Moved, std::format("moved the data of the former name: {} -> {}", from_s, to_s)};
  const std::string rename_error = ec.message();

  fs::create_directories(to, ec);
  std::string failed;
  auto note = [&](const fs::path& what, const std::error_code& e) {
    failed += std::format("{}{} ({})", failed.empty() ? "" : ", ", narrow(what.filename().wstring()), e.message());
  };
  for (const wchar_t* name : {L"config.json", L"state.json"}) {
    std::error_code e;
    if (!fs::exists(from / name, e)) continue;
    fs::copy_file(from / name, to / name, fs::copy_options::skip_existing, e);
    if (e) note(from / name, e);
  }
  if (std::error_code e; fs::is_directory(from / L"logs", e)) {
    fs::copy(from / L"logs", to / L"logs", fs::copy_options::recursive | fs::copy_options::skip_existing, e);
    if (e) note(from / L"logs", e);
  }
  if (!failed.empty())
    return {Kind::Failed, std::format("could not move {} ({}) nor copy all of it to {}: {}", from_s, rename_error, to_s, failed)};
  return {Kind::Copied, std::format("could not move {} ({}): copied config.json, state.json and logs to {}; the old folder stays",
                                    from_s, rename_error, to_s)};
}

}  // namespace ld
