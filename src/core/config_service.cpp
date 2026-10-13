// src/core/config_service.cpp
#include "core/config_service.h"
#include <format>
#include <utility>
#include "core/log.h"
#include "core/strings.h"

namespace ld {
using nlohmann::json;

namespace {
bool same_config(const Config& a, const Config& b) { return to_json(a) == to_json(b); }

bool same_errors(const std::vector<ConfigError>& a, const std::vector<ConfigError>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].path != b[i].path || a[i].message != b[i].message) return false;
  return true;
}
}  // namespace

ConfigService::ConfigService(std::filesystem::path file, ModuleLookup lookup, ConfigShape shape)
    : file_(std::move(file)), lookup_(std::move(lookup)), shape_(std::move(shape)) {}

const Config& ConfigService::load() {
  std::error_code ec;
  std::filesystem::path tmp = file_;
  tmp += ".tmp";
  std::filesystem::remove(tmp, ec);  // left over by a save interrupted by a crash
  LoadResult r = load_config_file(file_, lookup_, shape_);
  if (r.config) {
    config_ = std::move(*r.config);
    const std::string before = config_.model;
    fit_to_shape(config_, shape_);
    extra_ = unknown_keys(r.doc);
    if (r.migrated) persist_migration();
    else if (before != config_.model) persist_device(before);
    return config_;
  }
  config_ = default_config(shape_);
  if (r.missing) {
    if (!save_config_file(file_, config_)) log_error("cannot write the default config to {}", narrow(file_.wstring()));
    return config_;
  }
  for (const auto& e : r.errors) log_error("config {}: {}", e.path, e.message);
  log_warn("invalid config file left untouched; using the default layout");
  errors_ = std::move(r.errors);
  return config_;
}

json ConfigService::document() const { return with_unknown_keys(to_json(config_), extra_); }

UpdateResult ConfigService::update(const json& doc, std::uint64_t expected_revision) {
  if (expected_revision != 0 && expected_revision != revision_) return {UpdateStatus::Conflict, {}, revision_};
  ParseResult parsed = parse_config(doc, lookup_, shape_);
  if (!parsed.config) return {UpdateStatus::Invalid, std::move(parsed.errors), revision_};
  fit_to_shape(*parsed.config, shape_);
  if (auto beyond = slots_beyond_device(*parsed.config, config_, shape_); !beyond.empty())
    return {UpdateStatus::Invalid, std::move(beyond), revision_};
  json extra = unknown_keys(doc);
  // Only growth is refused: unknown keys already in the file (hand-written) come back with every save.
  if (const std::size_t size = extra.dump().size(); size > kMaxUnknownBytes && size > extra_.dump().size())
    return {UpdateStatus::Invalid,
            {{"", std::format("unknown keys take {} bytes (at most {}): remove some", size, kMaxUnknownBytes)}},
            revision_};
  if (same_config(*parsed.config, config_) && extra == extra_ && errors_.empty())
    return {UpdateStatus::Unchanged, {}, revision_};
  if (!save_config_file(file_, *parsed.config, extra)) {
    log_error("cannot write {}", narrow(file_.wstring()));
    return {UpdateStatus::WriteFailed, {{"", "cannot write config.json"}}, revision_};
  }
  commit(std::move(*parsed.config), std::move(extra));
  return {UpdateStatus::Applied, {}, revision_};
}

void ConfigService::reload() {
  LoadResult r = load_config_file(file_, lookup_, shape_);
  if (r.missing) {
    log_warn("config file deleted; writing the running configuration back");
    if (!save_config_file(file_, config_, extra_)) log_error("cannot write {}", narrow(file_.wstring()));
    return;
  }
  if (!r.config) {
    for (const auto& e : r.errors) log_error("config {}: {}", e.path, e.message);
    log_warn("config file rejected; keeping the running configuration");
    set_errors(std::move(r.errors));
    return;
  }
  fit_to_shape(*r.config, shape_);
  json extra = unknown_keys(r.doc);
  if (same_config(*r.config, config_)) {  // our own write coming back, or a no-op edit
    extra_ = std::move(extra);
    set_errors({});
    if (r.migrated) persist_migration();
    return;
  }
  log_info("config file changed; applying it");
  commit(std::move(*r.config), std::move(extra));
  if (r.migrated) persist_migration();
}

void ConfigService::persist_migration() {
  std::filesystem::path bak = file_;
  bak += ".v1.bak";
  if (!backup_once(file_, bak)) {
    log_error("cannot back up {} before migrating it; it stays in the old format", narrow(file_.wstring()));
    return;
  }
  if (save_config_file(file_, config_, extra_))
    log_info("config.json migrated to the design system format (backup: {})", narrow(bak.filename().wstring()));
  else
    log_error("cannot write the migrated config to {}", narrow(file_.wstring()));
}

void ConfigService::persist_device(const std::string& before) {
  std::string shown = before;  // control characters would forge log lines
  for (char& ch : shown)
    if (static_cast<unsigned char>(ch) < 0x20) ch = '?';
  if (save_config_file(file_, config_, extra_))
    log_info("config.json now goes with the {} (was \"{}\")", shape_.name, shown);
  else
    log_error("cannot write the device model to {}", narrow(file_.wstring()));
}

void ConfigService::commit(Config next, json extra) {
  Config before = std::exchange(config_, std::move(next));
  extra_ = std::move(extra);
  ++revision_;
  errors_.clear();
  if (on_applied) on_applied(before, config_);
  if (on_status) on_status();
}

void ConfigService::set_errors(std::vector<ConfigError> errors) {
  if (same_errors(errors, errors_)) return;
  errors_ = std::move(errors);
  if (on_status) on_status();
}

}  // namespace ld
