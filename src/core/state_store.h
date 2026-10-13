#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

namespace ld {

// Small persistent key -> JSON store (state.json next to config.json). Every set() saves atomically.
// A missing or corrupt file yields an empty store.
class StateStore {
public:
  explicit StateStore(std::filesystem::path file);
  nlohmann::json get(const std::string& key) const;  // null if absent
  void set(const std::string& key, nlohmann::json value);
  // Rebuilds the store from `sources` (new key -> key whose value it takes; "" = none): afterwards
  // it holds exactly those keys that got a value. Swaps are safe. Saves once, only if something changed.
  void rekey(const std::map<std::string, std::string>& sources);

private:
  void save();
  std::filesystem::path file_;
  nlohmann::json data_ = nlohmann::json::object();
};

}  // namespace ld
