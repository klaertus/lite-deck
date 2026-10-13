// src/sensors/sensor_block.h
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace ld::sensors {

inline constexpr wchar_t kMappingName[] = L"Global\\LiteDeckSensors";
// SYSTEM: all access; interactive users: read only.
inline constexpr wchar_t kMappingSddl[] = L"D:(A;;GA;;;SY)(A;;GR;;;IU)";
inline constexpr std::uint32_t kBlockVersion = 1;
inline constexpr std::int64_t kMaxAgeMs = 5000;

enum class Status : std::uint32_t {
  Ok = 0,
  Starting = 1,        // service running, no reading yet
  PawnIoMissing = 2,   // PawnIOLib.dll or the PawnIO driver is not installed
  ModuleMissing = 3,   // AMDFamily17.bin is not next to the service exe
  ModuleRejected = 4,  // pawnio_load refused the module
  UnsupportedCpu = 5,  // not an AMD family 17h/19h CPU
  ReadFailed = 6,      // pawnio_execute failed or returned an implausible value
  AccessDenied = 7,    // pawnio_open refused (not SYSTEM/administrator)
};

// Shared-memory layout. Version 1: never reorder, only append with a version bump.
struct SensorBlock {
  std::uint32_t version;
  std::uint32_t seq;  // odd while the writer is inside write_block
  double cpu_temp_c;
  std::int64_t updated_unix_ms;
  std::uint32_t status;  // a Status
};
static_assert(sizeof(SensorBlock) == 32);
static_assert(offsetof(SensorBlock, cpu_temp_c) == 8 && offsetof(SensorBlock, status) == 24);
static_assert(std::atomic_ref<double>::is_always_lock_free && std::atomic_ref<std::int64_t>::is_always_lock_free &&
              std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(alignof(SensorBlock) >= std::atomic_ref<double>::required_alignment &&
              alignof(SensorBlock) >= std::atomic_ref<std::int64_t>::required_alignment);

struct SensorSnapshot {
  std::uint32_t version = 0;
  double cpu_temp_c = 0;
  std::int64_t updated_unix_ms = 0;
  Status status = Status::Starting;
};

// Single writer only. Recovers from an odd seq left by a writer that died mid-write.
void write_block(SensorBlock& b, double temp_c, std::int64_t unix_ms, Status status);

// Lock-free consistent read; retries while a write is in progress or completed during the read.
// nullopt after `max_tries` attempts. `between` is a test hook run after the field reads.
std::optional<SensorSnapshot> read_block(const SensorBlock& b, int max_tries = 64,
                                         const std::function<void()>& between = {});

// The temperature to display, or nullopt: no snapshot, unknown version, status not Ok,
// or a timestamp more than `max_age_ms` away from now (in either direction).
std::optional<double> fresh_temperature(const std::optional<SensorSnapshot>& s, std::int64_t now_unix_ms,
                                        std::int64_t max_age_ms = kMaxAgeMs);

std::int64_t unix_now_ms();
std::string_view status_name(Status s);

}  // namespace ld::sensors
