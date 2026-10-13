#include <cmath>
#include <format>
#include <set>
#include <string_view>
#include "core/log.h"
#include "modules/all_modules.h"

namespace ld {

std::vector<std::string> element_spec_errors(std::span<const ElementSpec> elements) {
  std::vector<std::string> errors;
  std::set<std::string_view> keys;
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const ElementSpec& e = elements[i];
    const std::string name = e.key ? std::format("element \"{}\"", e.key) : std::format("element #{}", i);
    if (!e.key || !*e.key) errors.push_back(std::format("{}: no key", name));
    else if (!keys.insert(e.key).second) errors.push_back(std::format("{}: key used twice", name));
    if (!e.label) errors.push_back(std::format("{}: no label", name));
    if (e.icon != (e.kind == ElementKind::Icon)) errors.push_back(std::format("{}: icon must be set exactly for kind Icon", name));
    if (e.kind != ElementKind::Text && e.role != TextRole::None)
      errors.push_back(std::format("{}: an icon or a graph has no text role", name));
    if (e.kind == ElementKind::Graph) continue;  // no size or weight
    if (e.min_size > e.max_size) errors.push_back(std::format("{}: min_size {} > max_size {}", name, e.min_size, e.max_size));
    if (!std::isfinite(e.size)) errors.push_back(std::format("{}: size is not a number", name));
    if (e.weight != 400 && e.weight != 600 && e.weight != 700)
      errors.push_back(std::format("{}: weight {} (400, 600 or 700)", name, e.weight));
  }
  return errors;
}

std::vector<ModuleInfo> sound_modules(std::vector<ModuleInfo> modules) {
  std::vector<ModuleInfo> out;
  out.reserve(modules.size());
  for (ModuleInfo& m : modules) {
    const std::vector<std::string> errors = element_spec_errors(m.elements);
    if (errors.empty()) {
      out.push_back(std::move(m));
      continue;
    }
    for (const std::string& e : errors) log_error("module {}: {}; the module is not loaded", m.id, e);
  }
  return out;
}

const std::vector<ModuleInfo>& module_registry() {
  static const std::vector<ModuleInfo> registry = sound_modules({
      clock_module_info(),
      cpu_load_module_info(),
      cpu_temp_module_info(),
      ram_module_info(),
      gpu_module_info(),
      date_module_info(),
      weather_module_info(),
      sun_module_info(),
      crypto_gold_module_info(),
      stopwatch_module_info(),
      volume_module_info(),
      mic_module_info(),
      media_module_info(),
      seek_module_info(),
      now_playing_module_info(),
      notifications_module_info(),
      net_speed_module_info(),
      ping_module_info(),
      disk_module_info(),
      http_sensor_module_info(),
      launcher_module_info(),
      hotkey_module_info(),
  });
  return registry;
}

}  // namespace ld
