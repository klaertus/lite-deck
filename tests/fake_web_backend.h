// tests/fake_web_backend.h
#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "web/api.h"

// Answers directly (no loop) and records what the API asked. Thread-safe: the socket tests call it
// from web threads.
struct FakeWebBackend final : ld::WebBackend {
  std::mutex mu;
  ld::ConfigSnapshot snapshot{nlohmann::json{{"version", 1}}, 4};
  std::optional<ld::UpdateResult> next_update;  // unset: Applied with revision + 1
  bool silent = false;                          // behaves like a loop that never answers
  nlohmann::json last_doc;
  std::uint64_t last_expected = 0;
  int update_calls = 0;
  int last_preview = -99;
  std::vector<std::string> font_list{"Arial", "Segoe UI"};
  int font_calls = 0;
  std::vector<std::uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
  std::string tag;  // preview_tag(); "" = none

  std::optional<ld::ConfigSnapshot> config() override {
    std::lock_guard lock(mu);
    if (silent) return std::nullopt;
    return snapshot;
  }
  std::optional<ld::ConfigUpdate> update_config(nlohmann::json doc, std::uint64_t expected) override {
    std::lock_guard lock(mu);
    if (silent) return std::nullopt;
    ++update_calls;
    last_doc = doc;
    last_expected = expected;
    ld::UpdateResult r = next_update.value_or(ld::UpdateResult{ld::UpdateStatus::Applied, {}, snapshot.revision + 1});
    if (r.status == ld::UpdateStatus::Applied) snapshot = ld::ConfigSnapshot{doc, r.revision};
    return ld::ConfigUpdate{r, snapshot};
  }
  std::optional<std::string> preview(int target) override {
    std::lock_guard lock(mu);
    if (silent) return std::nullopt;
    last_preview = target;
    return std::string(jpeg.begin(), jpeg.end());
  }
  std::string preview_tag(int) override {
    std::lock_guard lock(mu);
    return tag;
  }
  std::vector<std::string> fonts() override {
    std::lock_guard lock(mu);
    ++font_calls;
    return font_list;
  }
  std::optional<nlohmann::json> status() override {
    std::lock_guard lock(mu);
    if (silent) return std::nullopt;
    return nlohmann::json{{"device", "connected"}, {"revision", snapshot.revision}, {"config_errors", nlohmann::json::array()}};
  }
};
