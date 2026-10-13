#include <doctest/doctest.h>
#include <windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include "core/config_watcher.h"
#include "core/event_loop.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
void write_text(const fs::path& p, const char* text) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}
}  // namespace

TEST_CASE("notify_mentions finds the file name case-insensitively and never reads past the end") {
  alignas(DWORD) std::byte buf[256]{};
  auto put = [&](std::size_t off, std::wstring_view name, DWORD next) {
    auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf + off);
    fni->NextEntryOffset = next;
    fni->Action = FILE_ACTION_MODIFIED;
    fni->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(fni->FileName, name.data(), fni->FileNameLength);
  };
  put(0, L"config.json.tmp", 64);
  put(64, L"Config.JSON", 0);
  const std::size_t full = 64 + offsetof(FILE_NOTIFY_INFORMATION, FileName) + 11 * sizeof(wchar_t);
  CHECK(ld::notify_mentions(buf, full, L"config.json"));
  CHECK_FALSE(ld::notify_mentions(buf, 64, L"config.json"));  // only the .tmp record
  CHECK_FALSE(ld::notify_mentions(buf, 70, L"config.json"));  // second record cut short
  CHECK_FALSE(ld::notify_mentions(buf, 0, L"config.json"));
}

TEST_CASE("ConfigWatcher debounces a burst of writes into one callback, 300 ms after the last") {
  fs::path dir = fs::temp_directory_path() / "ld_watch_burst";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_text(dir / "config.json", "{}");
  ld::EventLoop loop;
  int calls = 0;
  ld::TimePoint last_write{}, fired{};
  ld::ConfigWatcher w(loop, dir / "config.json", [&] {
    ++calls;
    fired = ld::Clock::now();
  });
  REQUIRE(w.active());
  const auto t0 = ld::Clock::now();
  for (int i = 0; i < 3; ++i)
    loop.at(t0 + 100ms + i * 60ms, [&] {
      write_text(dir / "config.json", "{\"v\":1}");
      last_write = ld::Clock::now();
    });
  loop.at(t0 + 200ms, [&] { write_text(dir / "other.txt", "x"); });  // another file: ignored
  loop.at(t0 + 1500ms, [&] { loop.quit(); });
  loop.run();
  CHECK(calls == 1);
  CHECK(fired - last_write >= 290ms);
  fs::remove_all(dir);
}

TEST_CASE("ConfigWatcher sees an atomic replace and ignores the temp file alone") {
  fs::path dir = fs::temp_directory_path() / "ld_watch_move";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_text(dir / "config.json", "{}");
  ld::EventLoop loop;
  int calls = 0;
  ld::ConfigWatcher w(loop, dir / "config.json", [&] { ++calls; });
  const auto t0 = ld::Clock::now();
  loop.at(t0 + 50ms, [&] { write_text(dir / "config.json.tmp", "{\"v\":2}"); });
  loop.at(t0 + 600ms, [&] {
    CHECK(calls == 0);  // the temp file alone is not the config
    MoveFileExW((dir / "config.json.tmp").c_str(), (dir / "config.json").c_str(), MOVEFILE_REPLACE_EXISTING);
  });
  loop.at(t0 + 1500ms, [&] { loop.quit(); });
  loop.run();
  CHECK(calls == 1);
  fs::remove_all(dir);
}

TEST_CASE("ConfigWatcher destroyed before its debounce never calls back") {
  fs::path dir = fs::temp_directory_path() / "ld_watch_dtor";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_text(dir / "config.json", "{}");
  // Control: the same write with a live watcher does fire, so the destroyed case below is not vacuous.
  {
    ld::EventLoop loop;
    int calls = 0;
    ld::ConfigWatcher w(loop, dir / "config.json", [&] { ++calls; });
    REQUIRE(w.active());
    const auto t0 = ld::Clock::now();
    loop.at(t0 + 50ms, [&] { write_text(dir / "config.json", "{\"v\":3}"); });
    loop.at(t0 + 1200ms, [&] { loop.quit(); });
    loop.run();
    CHECK(calls == 1);
  }
  ld::EventLoop loop;
  int calls = 0;
  auto w = std::make_unique<ld::ConfigWatcher>(loop, dir / "config.json", [&] { ++calls; });
  REQUIRE(w->active());
  const auto t0 = ld::Clock::now();
  loop.at(t0 + 50ms, [&] { write_text(dir / "config.json", "{\"v\":4}"); });
  loop.at(t0 + 200ms, [&] {
    CHECK(calls == 0);  // write seen at most; the 300 ms debounce has not elapsed
    w.reset();
  });
  loop.at(t0 + 1000ms, [&] { loop.quit(); });
  loop.run();
  CHECK(w == nullptr);
  CHECK(calls == 0);
  fs::remove_all(dir);
}

TEST_CASE("ConfigWatcher retries a folder that is missing at start, then reloads and watches") {
  fs::path dir = fs::temp_directory_path() / "ld_watch_retry";
  fs::remove_all(dir);
  ld::EventLoop loop;
  int calls = 0;
  ld::ConfigWatcher w(loop, dir / "config.json", [&] { ++calls; }, 100ms);
  CHECK_FALSE(w.active());
  const auto t0 = ld::Clock::now();
  loop.at(t0 + 150ms, [&] {
    fs::create_directories(dir);
    write_text(dir / "config.json", "{}");
  });
  loop.at(t0 + 800ms, [&] {
    CHECK(w.active());
    CHECK(calls == 1);  // once when the watch came back: edits made meanwhile were not seen
    write_text(dir / "config.json", "{\"v\":5}");
  });
  loop.at(t0 + 1500ms, [&] { loop.quit(); });
  loop.run();
  CHECK(calls == 2);
  fs::remove_all(dir);
}

TEST_CASE("ConfigWatcher on a missing folder is inactive and harmless") {
  ld::EventLoop loop;
  ld::ConfigWatcher w(loop, fs::temp_directory_path() / "ld_watch_nope" / "sub" / "config.json", [] {});
  CHECK_FALSE(w.active());
}
