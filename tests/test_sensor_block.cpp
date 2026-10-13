#include <doctest/doctest.h>
#include <atomic>
#include <ostream>
#include <thread>
#include "sensors/sensor_block.h"

using namespace ld::sensors;

TEST_CASE("sensor block: write then read returns the values and an even seq") {
  SensorBlock b{};
  write_block(b, 52.375, 1'000'000, Status::Ok);
  auto s = read_block(b);
  REQUIRE(s);
  CHECK(s->version == kBlockVersion);
  CHECK(s->cpu_temp_c == 52.375);
  CHECK(s->updated_unix_ms == 1'000'000);
  CHECK(s->status == Status::Ok);
  CHECK(b.seq % 2 == 0);
  CHECK(b.seq == 2);
}

TEST_CASE("sensor block: a zeroed block reads as version 0 and is never fresh") {
  SensorBlock b{};
  auto s = read_block(b);
  REQUIRE(s);
  CHECK(s->version == 0);
  CHECK_FALSE(fresh_temperature(s, 0));
}

TEST_CASE("sensor block: a writer stuck mid-write makes the reader give up") {
  SensorBlock b{};
  b.seq = 7;  // odd: a write in progress (or a writer that died mid-write)
  CHECK_FALSE(read_block(b, 10));
}

TEST_CASE("sensor block: a new writer recovers from an odd seq left by a dead one") {
  SensorBlock b{};
  b.seq = 7;
  write_block(b, 40.0, 5, Status::Ok);
  CHECK(b.seq % 2 == 0);
  CHECK(b.seq > 7);
  REQUIRE(read_block(b));
  CHECK(read_block(b)->cpu_temp_c == 40.0);
}

TEST_CASE("sensor block: a write during the read forces a retry and returns the new values") {
  SensorBlock b{};
  write_block(b, 40.0, 1000, Status::Ok);
  int calls = 0;
  auto s = read_block(b, 64, [&] {
    if (calls++ == 0) write_block(b, 41.0, 2000, Status::Ok);  // torn: first attempt must be rejected
  });
  REQUIRE(s);
  CHECK(calls == 2);
  CHECK(s->cpu_temp_c == 41.0);
  CHECK(s->updated_unix_ms == 2000);
}

TEST_CASE("sensor block: a write during every attempt yields nullopt") {
  SensorBlock b{};
  write_block(b, 40.0, 1000, Status::Ok);
  int calls = 0;
  CHECK_FALSE(read_block(b, 5, [&] { write_block(b, 40.0 + ++calls, 1000 + calls, Status::Ok); }));
  CHECK(calls == 5);
}

TEST_CASE("sensor block: concurrent writer and reader never see a torn pair") {
  SensorBlock b{};
  write_block(b, 0.0, 0, Status::Ok);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (std::int64_t i = 1; !stop.load(); ++i) {
      write_block(b, static_cast<double>(i), i * 1000, Status::Ok);
      std::this_thread::yield();  // pace the writer: a tight loop leaves seq odd and starves the reader
    }
  });
  int consistent = 0;
  int failed = 0;
  for (int i = 0; i < 50'000; ++i) {
    auto s = read_block(b, 1'000'000);
    if (!s || s->updated_unix_ms != static_cast<std::int64_t>(s->cpu_temp_c) * 1000) {
      ++failed;  // no REQUIRE here: a throw would leave the writer thread joinable and terminate
      break;
    }
    ++consistent;
  }
  stop.store(true);
  writer.join();
  CHECK(failed == 0);
  CHECK(consistent == 50'000);
}

TEST_CASE("fresh_temperature: ok and recent -> value; old, future, error or wrong version -> nullopt") {
  SensorSnapshot s{kBlockVersion, 55.5, 10'000, Status::Ok};
  CHECK(fresh_temperature(s, 10'000) == 55.5);
  CHECK(fresh_temperature(s, 15'000) == 55.5);    // exactly 5 s old: still fresh
  CHECK_FALSE(fresh_temperature(s, 15'001));      // > 5 s: stale (service stopped or hung)
  CHECK_FALSE(fresh_temperature(s, 4'999));       // > 5 s in the future: clock jumped, distrust
  CHECK_FALSE(fresh_temperature(std::nullopt, 10'000));
  auto bad = s;
  bad.status = Status::UnsupportedCpu;
  CHECK_FALSE(fresh_temperature(bad, 10'000));
  bad = s;
  bad.version = 2;
  CHECK_FALSE(fresh_temperature(bad, 10'000));
}

TEST_CASE("status_name covers every status") {
  CHECK(status_name(Status::Ok) == "ok");
  CHECK(status_name(Status::PawnIoMissing) == "pawnio-missing");
  CHECK(status_name(Status::UnsupportedCpu) == "unsupported-cpu");
  CHECK(status_name(static_cast<Status>(99)) == "unknown");
}

TEST_CASE("unix_now_ms is after 2026-01-01") {
  CHECK(unix_now_ms() > 1'767'225'600'000LL);
}
