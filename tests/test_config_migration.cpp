// tests/test_config_migration.cpp: the v1 config migration, on real-shaped configs.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "core/config.h"
#include "core/config_migration.h"
#include "core/config_service.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

json nulls(int n) {
  json a = json::array();
  for (int i = 0; i < n; ++i) a.push_back(nullptr);
  return a;
}

// The first default layout, no appearance, now_playing still had font_size.
json first_layout_config() {
  json keys = nulls(10);
  keys[0] = {{"module", "cpu_load"}, {"settings", {{"color", "#00e0a0"}}}};
  keys[2] = {{"module", "ram"}, {"settings", {{"color", "#7ab8ff"}}}};
  keys[3] = {{"module", "gpu"}, {"settings", {{"mode", "vram"}, {"color", "#76d275"}, {"index", 0}}}};
  keys[4] = {{"module", "gpu"}, {"settings", {{"mode", "temp"}, {"color", "#ff8a65"}, {"index", 0}}}};
  keys[5] = {{"module", "crypto_gold"}, {"settings", {{"color", "#f2a900"}}}};
  keys[6] = {{"module", "clock"}, {"settings", {{"color", "#ffffff"}, {"seconds", true}}}};
  keys[7] = {{"module", "weather"}, {"settings", {{"city", "Brussels"}, {"color", "#ffd36e"}}}};
  keys[8] = {{"module", "date"}, {"settings", {{"color", "#ffffff"}}}};
  keys[9] = {{"module", "stopwatch"}, {"settings", {{"file", ""}, {"color", "#00e080"}}}};
  return json{{"version", 1},
              {"device", {{"brightness", 60}, {"ring_brightness", 100},
                          {"ring_colors", {"#3e0000", "#002132", "#1b3500", "#3d1f00"}}}},
              {"web", {{"port", 7878}}},
              {"keys", keys},
              {"knobs", {{{"module", "volume"}, {"settings", {{"step", 2}}}},
                         {{"module", "mic"}, {"settings", {{"step", 2}}}},
                         {{"module", "media"}, {"settings", json::object()}}, nullptr}},
              {"strip", {{{"width", 430}, {"module", "now_playing"},
                          {"settings", {{"scroll_speed", 2}, {"color", "#ffffff"}, {"font_size", 0}}}},
                         {{"width", 370}, {"module", "notifications"},
                          {"settings", {{"scroll_speed", 2}, {"text_color", "#ffffff"}, {"bg_color", "#000000"}}}}}}};
}

// Later: colours chosen on the page (uppercase from a colour picker), no element overrides.
json picked_colours_config() {
  json j = first_layout_config();
  j["keys"][0]["settings"]["color"] = "#FF0000";  // chosen
  j["keys"][6]["settings"]["color"] = "#ffcc00";  // chosen
  j["keys"][7]["settings"]["color"] = "#FFD36E";  // the old default, in uppercase
  return j;
}

// Last v1 shape: the user's real config of 2026-10-12 (sizes overridden everywhere, a custom seek ring).
json last_v1_config() {
  return json::parse(R"J({
  "appearance": {"font": "Segoe UI", "italic": false},
  "device": {"brightness": 59, "ring_brightness": 55, "ring_colors": ["#3e0000", "#002132", "#1b3500", "#3d1f00"]},
  "keys": [
    {"elements": {"label": {"size": 22}, "temp": {"size": 22}, "value": {"size": 32}}, "module": "cpu_load",
     "settings": {"color": "#00e0a0", "show_temperature": true}},
    {"elements": {"label": {"size": 22}, "temp": {"size": 22}, "value": {"size": 32}}, "module": "gpu",
     "settings": {"color": "#ff8a65", "index": 0, "mode": "usage", "show_temperature": true}},
    {"elements": {"label": {"size": 22}, "unit": {"size": 18}}, "module": "ram", "settings": {"color": "#7ab8ff"}},
    {"elements": {"label": {"size": 22}, "unit": {"size": 18}}, "module": "gpu",
     "settings": {"color": "#76d275", "index": 0, "mode": "vram", "show_temperature": false}},
    {"elements": {"times": {"size": 32}}, "module": "sun",
     "settings": {"city": "Brussels", "color": "#ffb74d", "verify_days": 7}},
    {"elements": {"prices": {"size": 30}}, "module": "crypto_gold", "settings": {"color": "#f2a900"}},
    {"elements": {"time": {"size": 35}}, "module": "clock", "settings": {"color": "#ffffff", "seconds": true}},
    {"elements": {"icon": {"size": 30}, "range": {"size": 20, "weight": 600}, "temp": {"size": 35}},
     "module": "weather", "settings": {"city": "Brussels", "color": "#ffd36e"}},
    {"elements": {"day": {"size": 40}, "month": {"size": 22, "weight": 600}, "weekday": {"size": 22, "weight": 600}},
     "module": "date", "settings": {"color": "#ffffff"}},
    {"elements": {"label": {"size": 22}, "time": {"size": 30}}, "module": "stopwatch",
     "settings": {"color": "#00e080", "file": ""}}],
  "knobs": [
    {"module": "volume", "settings": {"step": 2}}, {"module": "mic", "settings": {"step": 2}},
    {"module": "media", "settings": {}},
    {"module": "seek", "settings": {"brightness": 100, "color": "#4d2200", "idle_brightness": 100,
                                    "idle_color": "#3d1f00", "step_s": 10}}],
  "strip": [
    {"elements": {"icon": {"size": 24}, "idle": {"weight": 700}, "title": {"size": 31}}, "module": "now_playing",
     "settings": {"color": "#ffffff", "scroll_speed": 2}, "width": 400},
    {"elements": {"age": {"size": 20}, "app": {"size": 31}, "count": {"size": 24}, "empty": {"size": 26, "weight": 700},
                  "status": {"size": 26}}, "module": "notifications",
     "settings": {"bg_color": "#000000", "scroll_speed": 2, "text_color": "#ffffff"}, "width": 400}],
  "version": 1,
  "web": {"port": 7878}})J");
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("a first-layout config: default colours are dropped, the rest is kept, no palette is written") {
  const ld::Migration m = ld::migrate_v1(first_layout_config());
  REQUIRE(m.changed);
  const json& d = m.doc;
  CHECK_FALSE(d.contains("appearance"));  // never written by the migration: the palette defaults stay in code
  for (int i : {0, 2, 3, 4, 5, 6, 7, 8, 9}) {
    INFO("key " << i);
    CHECK_FALSE(d["keys"][i]["settings"].contains("color"));
    CHECK_FALSE(d["keys"][i].contains("elements"));  // nothing was chosen
  }
  CHECK(d["keys"][4]["settings"]["mode"] == "temp");
  CHECK(d["strip"][0]["settings"]["color"] == "#ffffff");  // strip settings are not touched
  CHECK(d["strip"][0]["settings"]["font_size"] == 0);
  CHECK(d["device"] == first_layout_config()["device"]);
  auto r = ld::parse_config(d, ld::find_module);
  CHECK(r.errors.empty());
}

TEST_CASE("a chosen colour moves to the primary elements, a default one is dropped") {
  const json d = ld::migrate_v1(picked_colours_config()).doc;
  CHECK(d["keys"][0]["elements"]["value"]["color"] == "#ff0000");  // lower case, like the parser writes
  CHECK(d["keys"][0]["elements"]["graph"]["color"] == "#ff0000");  // the graph follows the value
  CHECK(d["keys"][6]["elements"]["time"]["color"] == "#ffcc00");
  CHECK_FALSE(d["keys"][7].contains("elements"));  // "#FFD36E" is the old default, whatever the case
  CHECK_FALSE(d["keys"][0]["settings"].contains("color"));
  auto r = ld::parse_config(d, ld::find_module);
  REQUIRE(r.config);
  CHECK(r.config->keys[0]->elements.at("value").color == ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{255, 0, 0}});
}

TEST_CASE("the user's last v1 config migrates without losing a size or a weight") {
  const json before = last_v1_config();
  const json d = ld::migrate_v1(before).doc;
  CHECK(d["keys"][0]["elements"]["label"]["size"] == 22);
  CHECK(d["keys"][0]["elements"]["value"]["size"] == 32);
  CHECK_FALSE(d["keys"][0]["elements"]["value"].contains("color"));  // #00e0a0 was the default
  CHECK_FALSE(d["keys"][1]["elements"]["value"].contains("color"));  // #ff8a65: the old gpu usage default
  CHECK(d["keys"][4]["elements"]["line1"]["size"] == 32);  // sun "times" renamed
  CHECK(d["keys"][4]["elements"]["line2"]["size"] == 32);
  CHECK_FALSE(d["keys"][4]["elements"].contains("times"));
  CHECK(d["keys"][4]["elements"]["status"]["size"] == 32);  // sun "times" also sizes the status
  for (const char* k : {"symbol1", "symbol2"}) {  // the symbols get no override
    INFO(k);
    CHECK_FALSE(d["keys"][4]["elements"].contains(k));
    CHECK_FALSE(d["keys"][5]["elements"].contains(k));
  }
  CHECK(d["keys"][5]["elements"]["line1"]["size"] == 30);  // crypto_gold "prices" renamed
  CHECK_FALSE(d["keys"][5]["elements"].contains("prices"));
  CHECK(d["keys"][7]["elements"]["range"] == json{{"size", 20}, {"weight", 600}});
  CHECK(d["keys"][8]["elements"]["weekday"] == json{{"size", 22}, {"weight", 600}});
  CHECK(d["knobs"][3] == before["knobs"][3]);  // the custom seek ring stays as it is
  CHECK(d["strip"] == before["strip"]);
  CHECK(d["appearance"] == before["appearance"]);  // no states written
  for (int i = 0; i < 10; ++i) {
    INFO("key " << i);
    CHECK_FALSE(d["keys"][i]["settings"].contains("color"));
  }
  auto r = ld::parse_config(d, ld::find_module);
  INFO(ld::errors_to_json(r.errors).dump());
  REQUIRE(r.config);
  CHECK(r.config->keys[4]->elements.at("line1").size == 32);
  CHECK(r.config->keys[9]->elements.at("time").size == 30);
  CHECK(r.config->keys[4]->elements.at("status").size == 32);
  CHECK(r.config->keys[5]->elements.at("line2").size == 30);
}

TEST_CASE("the migration is idempotent and leaves a migrated or odd document alone") {
  const ld::Migration once = ld::migrate_v1(picked_colours_config());
  const ld::Migration twice = ld::migrate_v1(once.doc);
  CHECK_FALSE(twice.changed);
  CHECK(twice.doc == once.doc);
  CHECK_FALSE(ld::migrate_v1(json::array()).changed);
  CHECK_FALSE(ld::migrate_v1(ld::to_json(ld::default_config())).changed);  // a new-format file: no rewrite
  json chosen_states = picked_colours_config();  // a page-written palette is no marker: old colours still move
  chosen_states["appearance"]["states"] = {{"warning", "#123456"}};
  const ld::Migration with_states = ld::migrate_v1(chosen_states);
  CHECK(with_states.changed);
  CHECK(with_states.doc["appearance"]["states"] == json{{"warning", "#123456"}});
  CHECK(with_states.doc["keys"][0]["elements"]["value"]["color"] == "#ff0000");
  json odd = picked_colours_config();
  odd["appearance"] = "Segoe UI";  // invalid: the parser reports it, the migration does not touch it
  CHECK_FALSE(ld::migrate_v1(odd).changed);
  json strange = picked_colours_config();
  strange["keys"][0]["settings"]["color"] = 12;  // not a colour: dropped, nothing moved
  const json d = ld::migrate_v1(strange).doc;
  CHECK_FALSE(d["keys"][0]["settings"].contains("color"));
  CHECK_FALSE(d["keys"][0].contains("elements"));
}

TEST_CASE("loading an old config writes config.json.v1.bak once and rewrites the file once") {
  const fs::path dir = fs::temp_directory_path() / "ld_migrate";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "config.json";
  const std::string original = picked_colours_config().dump(2);
  { std::ofstream(file, std::ios::binary) << original; }
  {
    ld::ConfigService svc(file, ld::find_module);
    svc.load();
    CHECK(svc.last_errors().empty());
  }
  const fs::path bak = dir / "config.json.v1.bak";
  REQUIRE(fs::exists(bak));
  CHECK(read_file(bak) == original);
  const json rewritten = json::parse(read_file(file));
  CHECK_FALSE(rewritten.value("appearance", json::object()).contains("states"));
  CHECK_FALSE(rewritten["keys"][0]["settings"].contains("color"));
  CHECK(rewritten["keys"][0]["elements"]["value"]["color"] == "#ff0000");
  const auto written_at = fs::last_write_time(file);
  {
    ld::ConfigService svc(file, ld::find_module);
    svc.load();  // already migrated: no rewrite
  }
  CHECK(fs::last_write_time(file) == written_at);
  // An old backup is never overwritten: restore the old file by hand and load again.
  { std::ofstream(file, std::ios::binary) << first_layout_config().dump(2); }
  {
    ld::ConfigService svc(file, ld::find_module);
    svc.load();
  }
  CHECK(read_file(bak) == original);
  fs::remove_all(dir);
}

TEST_CASE("an invalid old config is neither migrated on disk nor backed up") {
  const fs::path dir = fs::temp_directory_path() / "ld_migrate_bad";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "config.json";
  json bad = picked_colours_config();
  bad["device"]["brightness"] = 900;
  const std::string text = bad.dump(2);
  { std::ofstream(file, std::ios::binary) << text; }
  ld::ConfigService svc(file, ld::find_module);
  svc.load();
  CHECK_FALSE(svc.last_errors().empty());
  CHECK(read_file(file) == text);
  CHECK_FALSE(fs::exists(dir / "config.json.v1.bak"));
  fs::remove_all(dir);
}
