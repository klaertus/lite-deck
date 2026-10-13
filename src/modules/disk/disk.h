// src/modules/disk/disk.h: one drive, its space (used %, used / total GB) or its I/O rates.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "modules/common/worker.h"
#include "modules/module.h"

namespace ld {

struct DiskSpace {
  std::uint64_t total = 0, free = 0;  // bytes
};
// The space of a drive root ("C:\"); nullopt when the drive is absent. Runs on a worker (a sleeping
// or network drive may take seconds).
using DiskSpaceReader = std::function<std::optional<DiskSpace>(const std::wstring& root)>;

struct DiskIo {
  double read = 0, write = 0;  // bytes per second
};
// The I/O rates of the key's drive since the previous call; nullopt when the counters are not
// available (absent drive, first sample). Runs on a worker, once a second.
using DiskIoReader = std::function<std::optional<DiskIo>()>;

// "C:" from "c", "C:", "C:\" (spaces around ignored); nullopt for anything else.
std::optional<std::wstring> normalize_drive(const std::string& setting);
// MB/s (10^6 bytes), one decimal below 100: "3.2", "123".
std::wstring format_disk_rate(double bytes_per_second);

std::optional<DiskSpace> read_disk_space(const std::wstring& root);  // GetDiskFreeSpaceExW
// PDH \LogicalDisk(<drive>)\Disk Read Bytes/sec and Disk Write Bytes/sec, its own query.
DiskIoReader pdh_disk_io_reader(std::wstring drive);

// Empty arguments: read_disk_space, pdh_disk_io_reader(drive) and worker_runner().
std::unique_ptr<Module> make_disk_module(DiskSpaceReader space, DiskIoReader io, Runner runner);
ModuleInfo disk_module_info();

}  // namespace ld
