#include <doctest/doctest.h>
#include <windows.h>
#include <chrono>
#include <string>
#include <filesystem>
#include <fstream>
#include "core/log.h"
#include "device/hid.h"

TEST_CASE("open_hid_candidates skips one that fails, opens the next, and logs a failure reason once") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_hid_open";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "device") << "x"; }  // any file opens like a device interface here
  const ld::HidDeviceInfo missing{(dir / "missing").wstring(), 0, 0, 0, 65, 1025};
  const ld::HidDeviceInfo present{(dir / "device").wstring(), 0, 0, 0, 65, 1025};
  ld::log_init(dir / "test.log", ld::LogLevel::Warn);
  struct RestoreLog {
    ~RestoreLog() { ld::log_init({}, ld::LogLevel::Info); }
  } restore;
  CHECK(ld::open_hid_candidates({present}) != nullptr);  // known start: no failure logged yet
  CHECK(ld::open_hid_candidates({missing}) == nullptr);
  CHECK(ld::open_hid_candidates({missing}) == nullptr);  // same reason: not logged again
  CHECK(ld::open_hid_candidates({missing, present}) != nullptr);
  CHECK(ld::open_hid_candidates({missing}) == nullptr);  // after a success: logged again
  ld::log_init({}, ld::LogLevel::Info);
  std::string text;
  {
    std::ifstream in(dir / "test.log");
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  std::size_t count = 0;
  for (auto p = text.find("cannot open the deck (2)"); p != std::string::npos; p = text.find("cannot open the deck", p + 1))
    ++count;
  CHECK(count == 2);
  fs::remove_all(dir);
}

TEST_CASE("hid_enumerate returns nothing for a non-existent device") {
  CHECK(ld::hid_enumerate(0xFFFF, 0xFFFE).empty());
}

TEST_CASE("cancel_io_bounded cancels a pending read and returns promptly") {
  const std::wstring name = L"\\\\.\\pipe\\lite-deck-test-" + std::to_wstring(GetCurrentProcessId());
  HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE, 1, 64, 0, 0, nullptr);
  REQUIRE(server != INVALID_HANDLE_VALUE);
  HANDLE client = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  REQUIRE(client != INVALID_HANDLE_VALUE);
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  REQUIRE(ov.hEvent);
  char buf[16];
  const BOOL read_ok = ReadFile(client, buf, sizeof(buf), nullptr, &ov);  // the server never writes
  const DWORD read_err = GetLastError();
  REQUIRE_FALSE(read_ok);
  REQUIRE(read_err == ERROR_IO_PENDING);
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(ld::cancel_io_bounded(client, &ov, ld::Duration(500)));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400));
  CloseHandle(ov.hEvent);
  CloseHandle(client);
  CloseHandle(server);
}

TEST_CASE("cancel_io_bounded reports false when the operation stays pending") {
  const std::wstring name = L"\\\\.\\pipe\\lite-deck-test2-" + std::to_wstring(GetCurrentProcessId());
  HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE, 1, 64, 0, 0, nullptr);
  REQUIRE(server != INVALID_HANDLE_VALUE);
  HANDLE client = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  REQUIRE(client != INVALID_HANDLE_VALUE);
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  REQUIRE(ov.hEvent);
  ov.Internal = 0x103;  // STATUS_PENDING, but the handle has no such operation: CancelIoEx cannot finish it
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(ld::cancel_io_bounded(client, &ov, ld::Duration(50)));
  const auto dt = std::chrono::steady_clock::now() - t0;
  CHECK(dt >= std::chrono::milliseconds(40));
  CHECK(dt < std::chrono::milliseconds(500));
  // an operation that already completed counts as no longer pending
  ov.Internal = 0;
  SetEvent(ov.hEvent);
  CHECK(ld::cancel_io_bounded(client, &ov, ld::Duration(50)));
  CloseHandle(ov.hEvent);
  CloseHandle(client);
  CloseHandle(server);
}
