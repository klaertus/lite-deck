// tests/web_test_util.h
#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "modules/module.h"

namespace testutil {

struct NopModule final : ld::Module {
  void start(ld::ModuleContext&, const nlohmann::json&) override {}
  void render(ld::Canvas&) override {}
};

// A module with one text setting ("city"), allowed on keys and strip zones.
inline const ld::ModuleInfo& city_module() {
  static const ld::ModuleInfo info = [] {
    ld::ModuleInfo m;
    m.id = "city";
    m.name = "City";
    m.slots = {ld::SlotKind::Key, ld::SlotKind::StripZone};
    m.settings_schema = nlohmann::json::parse(
        R"({"fields":[{"key":"city","type":"string","default":"Brussels","label":"City"}]})");
    m.create = [] { return std::make_unique<NopModule>(); };
    return m;
  }();
  return info;
}

inline const ld::ModuleInfo* city_lookup(std::string_view id) { return id == "city" ? &city_module() : nullptr; }

// keys[0] = the city module with `city`; everything else empty; one 800 px zone.
inline nlohmann::json doc_with_city(const std::string& city) {
  nlohmann::json keys = nlohmann::json::array();
  for (int i = 0; i < 10; ++i) keys.push_back(nullptr);
  keys[0] = {{"module", "city"}, {"settings", {{"city", city}}}};
  return nlohmann::json{{"version", 1},
                        {"keys", keys},
                        {"knobs", {nullptr, nullptr, nullptr, nullptr}},
                        {"strip", {{{"width", 800}, {"module", nullptr}}}}};
}

inline std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

inline void write_text(const std::filesystem::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}

}  // namespace testutil
