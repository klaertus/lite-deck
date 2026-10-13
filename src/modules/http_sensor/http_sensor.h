// src/modules/http_sensor/http_sensor.h: one value from any JSON answer over http(s): a Home
// Assistant sensor, a NAS, a local API.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "modules/module.h"

namespace ld {

// One step of a JSON path: a key, or an index when `key` is empty.
struct JsonStep {
  std::string key;
  std::size_t index = 0;
};
// "result.temperature", "items[0].value", "[2]"; an optional leading "$" or "$."; "" = the whole
// answer. nullopt when the path cannot be read.
std::optional<std::vector<JsonStep>> parse_json_path(std::string_view path);
// The value at `path`, or nullptr when a step is missing or of the wrong kind.
const nlohmann::json* json_at(const nlohmann::json& root, const std::vector<JsonStep>& path);

ModuleInfo http_sensor_module_info();

}  // namespace ld
