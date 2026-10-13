#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "core/log.h"

namespace fs = std::filesystem;

TEST_CASE("log rotates files at the size limit and keeps N backups") {
  fs::path dir = fs::temp_directory_path() / "ld_log_test";
  fs::remove_all(dir);
  ld::log_init(dir / "app.log", ld::LogLevel::Info, 200, 2);
  for (int i = 0; i < 50; ++i) ld::log_info("message number {}", i);
  ld::log_debug("hidden");  // below min level
  CHECK(fs::exists(dir / "app.log"));
  CHECK(fs::exists(dir / "app.log.1"));
  CHECK(fs::exists(dir / "app.log.2"));
  CHECK_FALSE(fs::exists(dir / "app.log.3"));
  std::stringstream ss;
  {
    std::ifstream f(dir / "app.log");
    ss << f.rdbuf();
  }
  CHECK(ss.str().find("message number 49") != std::string::npos);
  CHECK(ss.str().find("hidden") == std::string::npos);
  ld::log_init({}, ld::LogLevel::Info);  // detach from the temp file
  fs::remove_all(dir);
}

namespace {
std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
// Holds the file open the way Get-Content -Wait does: read and write sharing, no delete sharing.
struct Holder {
  HANDLE h;
  Holder(const fs::path& p, DWORD share)
      : h(CreateFileW(p.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
  ~Holder() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};
}  // namespace

TEST_CASE("log rotates by copy and truncate while a viewer holds the file without delete sharing") {
  fs::path dir = fs::temp_directory_path() / "ld_log_held";
  fs::remove_all(dir);
  ld::log_init(dir / "app.log", ld::LogLevel::Info, 300, 2);
  for (int i = 0; i < 20; ++i) ld::log_info("before {:03}", i);
  REQUIRE(fs::exists(dir / "app.log.1"));
  {
    Holder viewer(dir / "app.log", FILE_SHARE_READ | FILE_SHARE_WRITE);
    REQUIRE(viewer.h != INVALID_HANDLE_VALUE);
    for (int i = 0; i < 40; ++i) {
      ld::log_info("held {:03}", i);
      CHECK(fs::file_size(dir / "app.log") <= 300);
      CHECK(fs::exists(dir / "app.log.1"));
    }
    CHECK(fs::exists(dir / "app.log.2"));
    CHECK_FALSE(fs::exists(dir / "app.log.3"));
    CHECK_FALSE(fs::exists(dir / "app.log.rotating"));
    // Nothing lost between the newest backup and the live file.
    const std::string all = read_all(dir / "app.log.1") + read_all(dir / "app.log");
    CHECK(all.find("held 039") != std::string::npos);
    CHECK(all.find("held 038") < all.find("held 039"));
  }
  ld::log_init({}, ld::LogLevel::Info);
  fs::remove_all(dir);
}

TEST_CASE("a failed log rotation keeps the backups and caps the live file") {
  fs::path dir = fs::temp_directory_path() / "ld_log_locked";
  fs::remove_all(dir);
  ld::log_init(dir / "app.log", ld::LogLevel::Info, 300, 2);
  for (int i = 0; i < 20; ++i) ld::log_info("before {:03}", i);
  REQUIRE(fs::exists(dir / "app.log.1"));
  REQUIRE(fs::exists(dir / "app.log.2"));
  {  // just over the limit, so the next line tries (and fails) to rotate
    const auto pad = 310 - fs::file_size(dir / "app.log");
    std::ofstream(dir / "app.log", std::ios::app | std::ios::binary) << std::string(pad - 1, 'x') << "\n";
  }
  REQUIRE(fs::file_size(dir / "app.log") == 310);
  const std::string b1 = read_all(dir / "app.log.1"), b2 = read_all(dir / "app.log.2");
  {
    // Read sharing only: neither rename nor truncation is possible (appending fails too).
    Holder locker(dir / "app.log", FILE_SHARE_READ);
    REQUIRE(locker.h != INVALID_HANDLE_VALUE);
    for (int i = 0; i < 40; ++i) ld::log_info("locked {:03}", i);
  }
  CHECK(read_all(dir / "app.log.1") == b1);
  CHECK(read_all(dir / "app.log.2") == b2);
  CHECK_FALSE(fs::exists(dir / "app.log.rotating"));
  // Unlocked, but within the retry delay: the file grows up to twice the limit, then stops.
  for (int i = 0; i < 40; ++i) ld::log_info("after {:03}", i);
  CHECK(fs::file_size(dir / "app.log") <= 600);
  CHECK(read_all(dir / "app.log.1") == b1);
  ld::log_init({}, ld::LogLevel::Info);
  fs::remove_all(dir);
}

TEST_CASE("format_log_time pads every field") {
  SYSTEMTIME st{};
  st.wYear = 2026; st.wMonth = 3; st.wDay = 7; st.wHour = 4; st.wMinute = 5; st.wSecond = 6; st.wMilliseconds = 9;
  CHECK(ld::format_log_time(st) == "2026-03-07 04:05:06.009");
}

TEST_CASE("log lines use local time") {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "ld_log_local";
  fs::remove_all(dir);
  ld::log_init(dir / "app.log", ld::LogLevel::Info);
  SYSTEMTIME before, after;
  GetLocalTime(&before);
  ld::log_info("hello");
  GetLocalTime(&after);
  std::string line;
  {
    std::ifstream f(dir / "app.log");
    std::getline(f, line);
  }
  ld::log_init({}, ld::LogLevel::Info);
  fs::remove_all(dir);
  std::string b = ld::format_log_time(before).substr(0, 16), a = ld::format_log_time(after).substr(0, 16);
  CHECK((line.substr(0, 16) == b || line.substr(0, 16) == a));
}
