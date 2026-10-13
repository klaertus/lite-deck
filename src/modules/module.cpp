#include "modules/module.h"
#include <algorithm>

namespace ld {

bool ModuleInfo::allows(SlotKind kind) const {
  return std::find(slots.begin(), slots.end(), kind) != slots.end();
}

const ModuleInfo* find_module(std::string_view id) {
  for (const auto& m : module_registry())
    if (m.id == id) return &m;
  return nullptr;
}

}  // namespace ld
