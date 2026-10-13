// src/sensors/shared_mapping.h
#pragma once
#include <memory>
#include "sensors/sensor_block.h"

namespace ld::sensors {

// Service side: creates (or reopens) the named section with `sddl` and maps it read-write.
class BlockWriter {
public:
  static std::unique_ptr<BlockWriter> create(const wchar_t* name, const wchar_t* sddl, unsigned long* error = nullptr);
  ~BlockWriter();
  BlockWriter(const BlockWriter&) = delete;
  BlockWriter& operator=(const BlockWriter&) = delete;
  SensorBlock& block() { return *block_; }

private:
  BlockWriter(void* mapping, SensorBlock* block) : mapping_(mapping), block_(block) {}
  void* mapping_;
  SensorBlock* block_;
};

// Daemon side: opens the section read-only. nullptr when it does not exist (service not running).
class BlockReader {
public:
  static std::unique_ptr<BlockReader> open(const wchar_t* name);
  ~BlockReader();
  BlockReader(const BlockReader&) = delete;
  BlockReader& operator=(const BlockReader&) = delete;
  const SensorBlock& block() const { return *block_; }

private:
  BlockReader(void* mapping, const SensorBlock* block) : mapping_(mapping), block_(block) {}
  void* mapping_;
  const SensorBlock* block_;
};

}  // namespace ld::sensors
