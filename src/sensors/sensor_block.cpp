// src/sensors/sensor_block.cpp
#include "sensors/sensor_block.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace ld::sensors {
namespace {
// atomic_ref<const T> only exists from C++26; the reader never stores through these refs.
template <class T> std::atomic_ref<T> ref(const T& v) { return std::atomic_ref<T>(const_cast<T&>(v)); }
constexpr auto relaxed = std::memory_order_relaxed;
}  // namespace

void write_block(SensorBlock& b, double temp_c, std::int64_t unix_ms, Status status) {
  auto seq = std::atomic_ref<std::uint32_t>(b.seq);
  const std::uint32_t s = seq.load(relaxed);
  const std::uint32_t begin = (s % 2 == 0) ? s + 1 : s + 2;  // odd
  seq.store(begin, relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::atomic_ref<std::uint32_t>(b.version).store(kBlockVersion, relaxed);
  std::atomic_ref<double>(b.cpu_temp_c).store(temp_c, relaxed);
  std::atomic_ref<std::int64_t>(b.updated_unix_ms).store(unix_ms, relaxed);
  std::atomic_ref<std::uint32_t>(b.status).store(static_cast<std::uint32_t>(status), relaxed);
  seq.store(begin + 1, std::memory_order_release);  // even again
}

std::optional<SensorSnapshot> read_block(const SensorBlock& b, int max_tries, const std::function<void()>& between) {
  for (int i = 0; i < max_tries; ++i) {
    const std::uint32_t s1 = ref(b.seq).load(std::memory_order_acquire);
    if (s1 % 2 != 0) {
      std::this_thread::yield();
      continue;
    }
    SensorSnapshot out;
    out.version = ref(b.version).load(relaxed);
    out.cpu_temp_c = ref(b.cpu_temp_c).load(relaxed);
    out.updated_unix_ms = ref(b.updated_unix_ms).load(relaxed);
    out.status = static_cast<Status>(ref(b.status).load(relaxed));
    if (between) between();
    std::atomic_thread_fence(std::memory_order_acquire);
    if (ref(b.seq).load(relaxed) == s1) return out;
  }
  return std::nullopt;
}

std::optional<double> fresh_temperature(const std::optional<SensorSnapshot>& s, std::int64_t now_unix_ms,
                                        std::int64_t max_age_ms) {
  if (!s || s->version != kBlockVersion || s->status != Status::Ok) return std::nullopt;
  // Distance computed in unsigned arithmetic: exact for any pair of int64 values, no signed overflow.
  const auto now = static_cast<std::uint64_t>(now_unix_ms);
  const auto then = static_cast<std::uint64_t>(s->updated_unix_ms);
  const std::uint64_t distance = now_unix_ms >= s->updated_unix_ms ? now - then : then - now;
  if (max_age_ms < 0 || distance > static_cast<std::uint64_t>(max_age_ms)) return std::nullopt;
  return s->cpu_temp_c;
}

std::int64_t unix_now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string_view status_name(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Starting: return "starting";
    case Status::PawnIoMissing: return "pawnio-missing";
    case Status::ModuleMissing: return "module-missing";
    case Status::ModuleRejected: return "module-rejected";
    case Status::UnsupportedCpu: return "unsupported-cpu";
    case Status::ReadFailed: return "read-failed";
    case Status::AccessDenied: return "access-denied";
  }
  return "unknown";
}

}  // namespace ld::sensors
