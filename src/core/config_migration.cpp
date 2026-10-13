// src/core/config_migration.cpp
#include "core/config_migration.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include "core/types.h"

namespace ld {
using nlohmann::json;

namespace {

struct Primary {
  const char* module;
  std::vector<const char*> elements;           // where a chosen colour goes
  std::vector<std::string_view> old_defaults;  // never chosen (the v1 schemas and default config)
};

const std::vector<Primary>& primaries() {
  static const std::vector<Primary> list = {
      {"cpu_load", {"value", "graph"}, {"#00e0a0"}},
      {"cpu_temp", {"value", "graph"}, {"#ff6e40"}},
      {"gpu", {"value", "graph"}, {"#76d275", "#ff8a65"}},
      {"ram", {"value"}, {"#7ab8ff"}},
      {"clock", {"time"}, {"#ffffff"}},
      {"date", {"day"}, {"#ffffff"}},
      {"stopwatch", {"time"}, {"#00e080"}},
      {"weather", {"temp"}, {"#ffd36e"}},
      {"crypto_gold", {"line1", "line2"}, {"#f2a900"}},
      {"sun", {"line1", "line2"}, {"#ffb74d"}},
  };
  return list;
}

struct Rename {
  const char* module;
  const char* from;
  std::vector<const char*> to;  // every element the old one sized
};

const std::vector<Rename>& renames() {
  static const std::vector<Rename> list = {
      {"crypto_gold", "prices", {"line1", "line2"}},
      {"sun", "times", {"line1", "line2", "status"}},
  };
  return list;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// The slot's elements object, created when absent; nullptr when it exists but is not an object
// (the parser reports it).
json* elements_of(json& slot) {
  auto it = slot.find("elements");
  if (it == slot.end() || it->is_null()) {
    slot["elements"] = json::object();
    return &slot["elements"];
  }
  return it->is_object() ? &*it : nullptr;
}

void rename_elements(json& slot, const Rename& r) {
  auto it = slot.find("elements");
  if (it == slot.end() || !it->is_object() || !it->contains(r.from)) return;
  const json old = it->at(r.from);
  it->erase(r.from);
  for (const char* to : r.to)
    if (!it->contains(to)) (*it)[to] = old;
}

void migrate_key(json& slot) {
  if (!slot.is_object()) return;
  auto mod = slot.find("module");
  if (mod == slot.end() || !mod->is_string()) return;
  const std::string id = mod->get<std::string>();
  for (const Rename& r : renames())
    if (id == r.module) rename_elements(slot, r);
  auto settings = slot.find("settings");
  if (settings == slot.end() || !settings->is_object() || !settings->contains("color")) return;
  const auto p = std::find_if(primaries().begin(), primaries().end(), [&](const Primary& x) { return id == x.module; });
  if (p == primaries().end()) return;  // not a key module of v1: left as it is
  const json value = settings->at("color");
  settings->erase("color");
  if (!value.is_string()) return;
  const std::string hex = lower(value.get<std::string>());
  if (!parse_color(hex)) return;
  if (std::find(p->old_defaults.begin(), p->old_defaults.end(), std::string_view(hex)) != p->old_defaults.end()) return;
  json* els = elements_of(slot);
  if (!els) return;
  for (const char* key : p->elements) {
    json& one = (*els)[key];
    if (!one.is_object()) one = json::object();
    if (!one.contains("color")) one["color"] = hex;
  }
}

}  // namespace

Migration migrate_v1(const json& doc) {
  Migration m{doc, false};
  if (!doc.is_object()) return m;
  if (auto app = doc.find("appearance"); app != doc.end() && !app->is_object()) return m;
  if (auto keys = m.doc.find("keys"); keys != m.doc.end() && keys->is_array())
    for (json& slot : *keys) migrate_key(slot);
  m.changed = m.doc != doc;
  return m;
}

}  // namespace ld
