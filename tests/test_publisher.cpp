#include <doctest/doctest.h>
#include <deque>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include "sensors/publisher.h"

using namespace ld::sensors;

namespace {
struct FakeSource : TempSource {
  std::deque<std::optional<double>>* values;
  explicit FakeSource(std::deque<std::optional<double>>* v) : values(v) {}
  std::optional<double> read_celsius() override {
    if (values->empty()) return 45.0;
    auto v = values->front();
    values->pop_front();
    return v;
  }
};

struct Harness {
  SensorBlock block{};
  int opens = 0;
  Status fail_with = Status::Ok;  // Ok = the opener succeeds
  std::deque<std::optional<double>> values;
  std::vector<Status> transitions;
  Publisher pub{block, [this]() -> OpenResult {
                  ++opens;
                  if (fail_with != Status::Ok) return {nullptr, fail_with};
                  return {std::make_unique<FakeSource>(&values), Status::Ok};
                }};
  Harness() {
    pub.on_status = [this](Status s, const std::string&) { transitions.push_back(s); };
  }
  SensorSnapshot snap() { return *read_block(block); }
};
}  // namespace

TEST_CASE("publisher starts as Starting with no timestamp") {
  Harness h;
  CHECK(h.snap().status == Status::Starting);
  CHECK(h.snap().updated_unix_ms == 0);
}

TEST_CASE("publisher opens once and publishes every tick") {
  Harness h;
  h.values = {50.0, 51.5};
  h.pub.tick(1000);
  CHECK(h.snap().cpu_temp_c == 50.0);
  CHECK(h.snap().updated_unix_ms == 1000);
  h.pub.tick(2000);
  CHECK(h.snap().cpu_temp_c == 51.5);
  CHECK(h.snap().status == Status::Ok);
  CHECK(h.opens == 1);
  CHECK(h.transitions == std::vector<Status>{Status::Ok});
}

TEST_CASE("publisher retries a failed open every 30 s and logs the failure once") {
  Harness h;
  h.fail_with = Status::PawnIoMissing;
  for (std::int64_t t = 0; t <= 29'000; t += 1000) h.pub.tick(t);
  CHECK(h.opens == 1);
  CHECK(h.snap().status == Status::PawnIoMissing);
  h.pub.tick(30'000);
  CHECK(h.opens == 2);
  CHECK(h.transitions == std::vector<Status>{Status::PawnIoMissing});
  h.fail_with = Status::Ok;
  h.pub.tick(31'000);
  CHECK(h.opens == 2);  // still waiting for the next 30 s slot
  h.pub.tick(60'000);
  CHECK(h.opens == 3);
  CHECK(h.snap().status == Status::Ok);
  CHECK(h.transitions == std::vector<Status>{Status::PawnIoMissing, Status::Ok});
}

TEST_CASE("publisher reports an unsupported CPU and keeps running") {
  Harness h;
  h.fail_with = Status::UnsupportedCpu;
  h.pub.tick(0);
  CHECK(h.snap().status == Status::UnsupportedCpu);
  CHECK(h.pub.status() == Status::UnsupportedCpu);
}

TEST_CASE("publisher drops the source after a failed read and reopens 30 s later") {
  Harness h;
  h.values = {50.0, std::nullopt};
  h.pub.tick(0);
  h.pub.tick(1000);
  CHECK(h.snap().status == Status::ReadFailed);
  h.pub.tick(2000);
  CHECK(h.opens == 1);
  h.pub.tick(31'000);
  CHECK(h.opens == 2);
  CHECK(h.snap().status == Status::Ok);
  CHECK(h.transitions == std::vector<Status>{Status::Ok, Status::ReadFailed, Status::Ok});
}

TEST_CASE("publisher rejects implausible values") {
  Harness h;
  h.values = {-49.0};  // raw register 0 with the range-select bit: not a real temperature
  h.pub.tick(0);
  CHECK(h.snap().status == Status::ReadFailed);
  Harness h2;
  h2.values = {200.0};
  h2.pub.tick(0);
  CHECK(h2.snap().status == Status::ReadFailed);
}

TEST_CASE("publisher accepts the boundaries and rejects just outside them") {
  for (double v : {-20.0, 150.0}) {
    Harness h;
    h.values = {v};
    h.pub.tick(0);
    CHECK(h.snap().status == Status::Ok);
    CHECK(h.snap().cpu_temp_c == v);
  }
  for (double v : {-20.1, 150.1}) {
    Harness h;
    h.values = {v};
    h.pub.tick(0);
    CHECK(h.snap().status == Status::ReadFailed);
    CHECK(h.snap().cpu_temp_c != v);
  }
}

TEST_CASE("publisher rejects NaN and infinity") {
  for (double v : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
    Harness h;
    h.values = {v};
    h.pub.tick(0);
    CHECK(h.snap().status == Status::ReadFailed);
    CHECK(std::isfinite(h.snap().cpu_temp_c));
  }
}

TEST_CASE("publisher treats a throwing opener as an open failure") {
  SensorBlock block{};
  int opens = 0;
  Publisher pub{block, [&]() -> OpenResult {
                  ++opens;
                  throw std::runtime_error("boom");
                }};
  std::string detail;
  pub.on_status = [&](Status, const std::string& d) { detail = d; };
  pub.tick(0);
  CHECK(pub.status() == Status::ReadFailed);
  CHECK(detail.find("open threw") != std::string::npos);
  CHECK(detail.find("boom") != std::string::npos);
  pub.tick(1000);
  CHECK(opens == 1);
  pub.tick(30'000);
  CHECK(opens == 2);
}

TEST_CASE("publisher treats a throwing read as a failed read") {
  struct Thrower : TempSource {
    std::optional<double> read_celsius() override { throw std::runtime_error("x"); }
  };
  SensorBlock block{};
  int opens = 0;
  Publisher pub{block, [&]() -> OpenResult {
                  ++opens;
                  return {std::make_unique<Thrower>(), Status::Ok};
                }};
  pub.tick(0);
  CHECK(pub.status() == Status::ReadFailed);
  pub.tick(1000);
  CHECK(opens == 1);
  pub.tick(30'000);
  CHECK(opens == 2);
}

TEST_CASE("publisher clamps the retry wait when the clock jumps back") {
  Harness h;
  h.fail_with = Status::PawnIoMissing;
  h.pub.tick(1'000'000);
  CHECK(h.opens == 1);
  h.pub.tick(10'000);  // clock went back by ~16 min: retry at once instead of waiting
  CHECK(h.opens == 2);
  h.pub.tick(20'000);
  CHECK(h.opens == 2);
  h.pub.tick(40'000);
  CHECK(h.opens == 3);
}
