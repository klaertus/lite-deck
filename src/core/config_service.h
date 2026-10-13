// src/core/config_service.h
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/config.h"

namespace ld {

enum class UpdateStatus { Applied, Unchanged, Invalid, Conflict, WriteFailed };

struct UpdateResult {
  UpdateStatus status = UpdateStatus::Unchanged;
  std::vector<ConfigError> errors;  // Invalid, WriteFailed
  std::uint64_t revision = 0;       // the revision in force after the call
};

// The live configuration: the copy in memory, the file and the keys of the file it does not know.
// Every applied change gets a new revision (the web API's ETag). Loop thread only: the web server
// reaches it through LoopBridge.
class ConfigService {
public:
  // update() refuses a document whose unknown keys (kept and written back) take more than this,
  // unless they take no more than those already kept (a hand-written file).
  static constexpr std::size_t kMaxUnknownBytes = 4096;

  // `shape` is the active device's (registry.h: config_shape); the default is the N4 Pro.
  ConfigService(std::filesystem::path file, ModuleLookup lookup, ConfigShape shape = {});
  const ConfigShape& shape() const { return shape_; }

  // Startup. Removes a leftover <file>.tmp. Missing file -> writes the default. Invalid file ->
  // default in memory, file untouched, last_errors() set.
  const Config& load();
  const Config& current() const { return config_; }
  std::uint64_t revision() const { return revision_; }
  nlohmann::json document() const;  // current config + the file's unknown keys

  // From the web page: revision check (0 = none), validation, write, then apply.
  UpdateResult update(const nlohmann::json& doc, std::uint64_t expected_revision);
  // From the file watcher: applies the file if it changed. Invalid -> keeps the running config and
  // sets last_errors(). Deleted -> writes the running config back.
  void reload();
  const std::vector<ConfigError>& last_errors() const { return errors_; }

  std::function<void(const Config& before, const Config& after)> on_applied;
  std::function<void()> on_status;  // revision or last_errors() changed

private:
  void commit(Config next, nlohmann::json extra);
  void set_errors(std::vector<ConfigError> errors);
  // After a load that migrated an old file: config.json.v1.bak (once), then the file in the new format.
  void persist_migration();
  // After a load whose model was not the active device's: the file gets the new model, once.
  void persist_device(const std::string& before);

  std::filesystem::path file_;
  ModuleLookup lookup_;
  ConfigShape shape_;
  Config config_;
  nlohmann::json extra_ = nlohmann::json::object();
  std::uint64_t revision_ = 1;
  std::vector<ConfigError> errors_;
};

}  // namespace ld
