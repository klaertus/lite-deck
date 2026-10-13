#include <doctest/doctest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "core/config.h"
#include "core/config_service.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct FakeMod : ld::Module {
  void start(ld::ModuleContext&, const json&) override {}
  void render(ld::Canvas&) override {}
};
ld::ModuleInfo make_info(std::string id, std::vector<ld::SlotKind> slots) {
  ld::ModuleInfo m;
  m.id = id;
  m.name = id;
  m.slots = std::move(slots);
  m.settings_schema = json::parse(R"({"fields":[{"key":"color","type":"color","default":"#ffffff","label":"c"}]})");
  m.elements = {ld::ElementSpec{.key = "value", .label = "Value", .size = 30, .weight = 700, .min_size = 10,
                                .max_size = 80, .gradient = true},
                ld::ElementSpec{"icon", "Icon", 26, 400, 10, 80, true, ld::ElementKind::Icon},
                ld::ElementSpec{.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8,
                                .max_size = 8, .kind = ld::ElementKind::Graph, .row = ld::Row::Bottom}};
  m.create = [] { return std::make_unique<FakeMod>(); };
  return m;
}
const ld::ModuleInfo kKeyMod = make_info("keymod", {ld::SlotKind::Key});
const ld::ModuleInfo kZoneMod = make_info("zonemod", {ld::SlotKind::StripZone});
ld::ModuleInfo make_pair_info() {  // a two-line key, like sun and crypto_gold
  ld::ModuleInfo m = make_info("pairmod", {ld::SlotKind::Key});
  m.elements = {ld::ElementSpec{.key = "line1", .label = "Line 1", .size = 26, .weight = 700, .min_size = 9,
                                .max_size = 40, .row = ld::Row::Line1},
                ld::ElementSpec{.key = "line2", .label = "Line 2", .size = 26, .weight = 700, .min_size = 9,
                                .max_size = 40, .row = ld::Row::Line2}};
  return m;
}
const ld::ModuleInfo kPairMod = make_pair_info();
ld::ModuleInfo make_checked_info() {  // refuses red, which its schema alone accepts
  ld::ModuleInfo m = make_info("checkedmod", {ld::SlotKind::Key});
  m.check = [](const json& s) -> std::optional<std::string> {
    if (s.at("color") == "#ff0000") return std::string("color: red is not allowed");
    if (s.at("color") == "#000000") (void)s.at("missing");  // a buggy check: throws
    return std::nullopt;
  };
  return m;
}
const ld::ModuleInfo kCheckedMod = make_checked_info();
const ld::ModuleLookup kLookup = [](std::string_view id) -> const ld::ModuleInfo* {
  if (id == "checkedmod") return &kCheckedMod;
  if (id == "keymod") return &kKeyMod;
  if (id == "zonemod") return &kZoneMod;
  if (id == "pairmod") return &kPairMod;
  return nullptr;
};

json valid_json() {
  json keys = json::array();
  for (int i = 0; i < 10; ++i) keys.push_back(nullptr);
  keys[0] = {{"module", "keymod"}, {"settings", {{"color", "#00ff00"}}}};
  return json{{"version", 1},
              {"device", {{"brightness", 70}, {"ring_brightness", 50},
                          {"ring_colors", {"#3e0000", "#002132", "#1b3500", "#3d1f00"}}}},
              {"web", {{"port", 7878}}},
              {"keys", keys},
              {"knobs", {nullptr, nullptr, nullptr, nullptr}},
              {"strip", {{{"width", 430}, {"module", "zonemod"}}, {{"width", 370}, {"module", nullptr}}}}};
}
}  // namespace

TEST_CASE("parse_config reads a valid document and applies setting defaults") {
  auto r = ld::parse_config(valid_json(), kLookup);
  REQUIRE(r.config);
  CHECK(r.errors.empty());
  CHECK(r.config->brightness == 70);
  CHECK(r.config->ring_colors[1] == ld::Rgb{0, 0x21, 0x32});
  REQUIRE(r.config->keys[0]);
  CHECK(r.config->keys[0]->module == "keymod");
  CHECK(r.config->keys[0]->settings["color"] == "#00ff00");
  CHECK_FALSE(r.config->keys[1]);
  REQUIRE(r.config->strip.size() == 2);
  CHECK(r.config->strip[0].slot->settings["color"] == "#ffffff");
  CHECK_FALSE(r.config->strip[1].slot);
}

TEST_CASE("parse_config reports precise errors") {
  json j = valid_json();
  j["keys"][3] = {{"module", "zonemod"}};       // not allowed on a key
  j["keys"][4] = {{"module", "nope"}};          // unknown
  j["strip"][1]["width"] = 300;                 // sum != 800
  j["device"]["brightness"] = 150;              // out of range
  auto r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  auto has = [&](const std::string& path) {
    return std::any_of(r.errors.begin(), r.errors.end(), [&](const ld::ConfigError& e) { return e.path == path; });
  };
  CHECK(has("keys[3].module"));
  CHECK(has("keys[4].module"));
  CHECK(has("strip"));
  CHECK(has("device.brightness"));
}

TEST_CASE("parse_config runs the module's own check on its settings, after the schema") {
  json j = valid_json();
  j["keys"][2] = {{"module", "checkedmod"}, {"settings", {{"color", "#ff0000"}}}};
  auto r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  REQUIRE(r.errors.size() == 1);
  CHECK(r.errors[0].path == "keys[2].settings");
  CHECK(r.errors[0].message == "color: red is not allowed");
  j["keys"][2]["settings"]["color"] = "#00ff00";
  r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.config->keys[2]->settings["color"] == "#00ff00");
  j["keys"][2]["settings"].erase("color");  // the default, checked too
  CHECK(ld::parse_config(j, kLookup).config);
  j["keys"][2]["settings"]["color"] = "#000000";  // the check throws: an error, not an exception
  ld::ParseResult thrown;
  CHECK_NOTHROW(thrown = ld::parse_config(j, kLookup));
  CHECK_FALSE(thrown.config);
  REQUIRE(thrown.errors.size() == 1);
  CHECK(thrown.errors[0].path == "keys[2].settings");
  CHECK(thrown.errors[0].message.starts_with("cannot be checked: "));
}

TEST_CASE("parse_config rejects over-long lists, narrow zones and bad version") {
  json j = valid_json();
  j["keys"] = json::array();
  for (int i = 0; i < 65; ++i) j["keys"].push_back(nullptr);
  CHECK_FALSE(ld::parse_config(j, kLookup).config);
  j = valid_json();
  j["knobs"] = json::array();
  for (int i = 0; i < 17; ++i) j["knobs"].push_back(nullptr);
  CHECK_FALSE(ld::parse_config(j, kLookup).config);
  j = valid_json();
  j["keys"] = "none";
  CHECK_FALSE(ld::parse_config(j, kLookup).config);
  j = valid_json();
  j["strip"] = {{{"width", 40}, {"module", nullptr}}, {{"width", 760}, {"module", nullptr}}};
  CHECK_FALSE(ld::parse_config(j, kLookup).config);
  j = valid_json();
  j["version"] = 2;
  CHECK_FALSE(ld::parse_config(j, kLookup).config);
}

TEST_CASE("to_json round-trips through parse_config") {
  auto c = ld::parse_config(valid_json(), kLookup).config.value();
  auto again = ld::parse_config(ld::to_json(c), kLookup);
  REQUIRE(again.config);
  CHECK(ld::to_json(*again.config) == ld::to_json(c));
}

TEST_CASE("save and load config file atomically") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_test";
  fs::remove_all(dir);
  auto c = ld::parse_config(valid_json(), kLookup).config.value();
  REQUIRE(ld::save_config_file(dir / "config.json", c));
  CHECK_FALSE(fs::exists(dir / "config.json.tmp"));
  auto r = ld::load_config_file(dir / "config.json", kLookup);
  REQUIRE(r.config);
  CHECK(r.config->brightness == 70);
  auto missing = ld::load_config_file(dir / "absent.json", kLookup);
  CHECK(missing.missing);
  CHECK_FALSE(missing.config);
  fs::remove_all(dir);
}

TEST_CASE("ConfigService::load keeps a broken file untouched and uses the default") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_broken";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "config.json") << "{ \"version\": 1, oops"; }
  ld::ConfigService svc(dir / "config.json", kLookup);
  ld::Config c = svc.load();
  CHECK(c.brightness == ld::default_config().brightness);
  CHECK_FALSE(svc.last_errors().empty());
  std::string content;
  {
    std::ifstream f(dir / "config.json");
    content.assign(std::istreambuf_iterator<char>(f), {});
  }
  CHECK(content == "{ \"version\": 1, oops");
  fs::remove_all(dir);
}

TEST_CASE("ConfigService::load writes the default when the file and its folder are missing") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_missing";
  fs::remove_all(dir);
  ld::ConfigService svc(dir / "config.json", kLookup);
  svc.load();
  CHECK(fs::exists(dir / "config.json"));
  fs::remove_all(dir);
}

TEST_CASE("a deeply nested config.json is rejected as invalid, without overflowing the stack") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_deep";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string base = valid_json().dump();
  auto with_nesting = [&](std::size_t levels) {  // an unknown key holding `levels` nested arrays
    return base.substr(0, base.size() - 1) + ",\"zz\":" + std::string(levels, '[') + std::string(levels, ']') + "}";
  };
  { std::ofstream(dir / "config.json", std::ios::binary) << with_nesting(100000); }
  auto r = ld::load_config_file(dir / "config.json", kLookup);
  CHECK_FALSE(r.config);
  CHECK_FALSE(r.missing);
  REQUIRE(r.errors.size() == 1);
  CHECK(r.errors[0].message == "JSON nested too deeply");
  { std::ofstream(dir / "config.json", std::ios::binary | std::ios::trunc) << with_nesting(60); }
  CHECK(ld::load_config_file(dir / "config.json", kLookup).config);  // below the limit: loads
  fs::remove_all(dir);
}

namespace {
bool has_path(const ld::ParseResult& r, const std::string& path) {
  return std::any_of(r.errors.begin(), r.errors.end(), [&](const ld::ConfigError& e) { return e.path == path; });
}
}  // namespace

TEST_CASE("parse_config never throws on wrong types") {
  json j = valid_json();
  j["version"] = "1";
  ld::ParseResult r;
  CHECK_NOTHROW(r = ld::parse_config(j, kLookup));
  CHECK_FALSE(r.config);
  CHECK(has_path(r, "version"));
  j = valid_json();
  j["device"] = 5;
  r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  CHECK(has_path(r, "device"));
  j = valid_json();
  j["web"] = "x";
  r = ld::parse_config(j, kLookup);
  CHECK(has_path(r, "web"));
  j = valid_json();
  j["device"]["brightness"] = 4294967396ULL;
  r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  CHECK(has_path(r, "device.brightness"));
}

TEST_CASE("malformed zone yields exactly one error") {
  json j = valid_json();
  j["strip"] = {{{"width", "x"}, {"module", nullptr}}, {{"width", 800}, {"module", nullptr}}};
  auto r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  CHECK(r.errors.size() == 1);
}

TEST_CASE("load_config_file accepts a UTF-8 BOM") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_bom";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "config.json", std::ios::binary) << "\xEF\xBB\xBF" << valid_json().dump(); }
  auto r = ld::load_config_file(dir / "config.json", kLookup);
  CHECK(r.config);
  fs::remove_all(dir);
}

TEST_CASE("ConfigService::load logs non-ANSI paths without throwing") {
  fs::path dir = fs::temp_directory_path() / L"ld_cfg_日本";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "X") << "file"; }
  // The parent of config.json is a file: the default cannot be written and the path is logged.
  ld::ConfigService svc(dir / "X" / "config.json", kLookup);
  CHECK_NOTHROW(svc.load());
  fs::remove_all(dir);
}

TEST_CASE("unknown_keys keeps only what the parser ignores, and with_unknown_keys puts it back") {
  json doc = valid_json();
  doc["comment"] = "keep";
  doc["device"]["future"] = 1;
  doc["web"]["bind"] = "x";
  json extra = ld::unknown_keys(doc);
  CHECK(extra == json{{"comment", "keep"}, {"device", {{"future", 1}}}, {"web", {{"bind", "x"}}}});
  auto c = ld::parse_config(doc, kLookup).config.value();
  json merged = ld::with_unknown_keys(ld::to_json(c), extra);
  CHECK(merged["comment"] == "keep");
  CHECK(merged["device"]["future"] == 1);
  CHECK(merged["device"]["brightness"] == 70);              // known keys come from the config
  CHECK(ld::with_unknown_keys(ld::to_json(c), json{{"version", 9}})["version"] == 1);  // never overwrites
  CHECK(ld::unknown_keys(json::array()).empty());
}

TEST_CASE("save_config_file keeps unknown keys and replaces a stale temp file") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_extra";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "config.json.tmp") << "stale garbage from a crash"; }
  auto c = ld::parse_config(valid_json(), kLookup).config.value();
  REQUIRE(ld::save_config_file(dir / "config.json", c, json{{"comment", "keep"}}));
  CHECK_FALSE(fs::exists(dir / "config.json.tmp"));
  auto r = ld::load_config_file(dir / "config.json", kLookup);
  REQUIRE(r.config);
  CHECK(r.doc["comment"] == "keep");
  CHECK(r.doc["device"]["brightness"] == 70);
  fs::remove_all(dir);
}

TEST_CASE("save_config_file fails cleanly when the target cannot be replaced") {
  fs::path dir = fs::temp_directory_path() / "ld_cfg_dirtarget";
  fs::remove_all(dir);
  fs::create_directories(dir / "config.json");  // a directory where the file should be
  auto c = ld::parse_config(valid_json(), kLookup).config.value();
  CHECK_FALSE(ld::save_config_file(dir / "config.json", c));
  CHECK_FALSE(fs::exists(dir / "config.json.tmp"));
  fs::remove_all(dir);
}

TEST_CASE("errors_to_json lists path and message") {
  json j = ld::errors_to_json({{"strip", "sum"}, {"keys[0].module", "unknown"}});
  CHECK(j == json::array({json{{"path", "strip"}, {"message", "sum"}},
                          json{{"path", "keys[0].module"}, {"message", "unknown"}}}));
}

TEST_CASE("a config without appearance keys gets Segoe UI, upright, and no per-slot overrides") {
  auto r = ld::parse_config(valid_json(), kLookup);
  REQUIRE(r.config);
  CHECK(r.config->appearance.font == "Segoe UI");
  CHECK_FALSE(r.config->appearance.italic);
  CHECK(r.config->keys[0]->font.empty());
  CHECK_FALSE(r.config->keys[0]->italic.has_value());
  CHECK(r.config->keys[0]->elements.empty());
  json out = ld::to_json(*r.config);
  CHECK(out["appearance"]["font"] == "Segoe UI");
  CHECK(out["appearance"]["italic"] == false);
  CHECK_FALSE(out["appearance"].contains("states"));  // defaults stay implicit
  CHECK(r.config->appearance.states == ld::StatePalette{});
  CHECK_FALSE(out["keys"][0].contains("font"));
  CHECK_FALSE(out["keys"][0].contains("italic"));
  CHECK_FALSE(out["keys"][0].contains("elements"));
  CHECK(ld::to_json(ld::default_config())["appearance"]["font"] == "Segoe UI");
}

TEST_CASE("fonts, italic and element overrides round-trip, globally and per slot") {
  json j = valid_json();
  j["appearance"] = {{"font", "Consolas"}, {"italic", true}};
  j["keys"][0]["font"] = "Bahnschrift";
  j["keys"][0]["italic"] = false;
  j["keys"][0]["elements"] = {{"value", {{"size", 44}, {"weight", 600}}}, {"icon", {{"size", 30}}}};
  j["strip"][0]["font"] = "Cascadia Code";
  j["strip"][0]["elements"] = {{"value", {{"weight", 400}}}};
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.config->appearance.font == "Consolas");
  CHECK(r.config->appearance.italic);
  const ld::SlotConfig& k = *r.config->keys[0];
  CHECK(k.font == "Bahnschrift");
  REQUIRE(k.italic.has_value());
  CHECK_FALSE(*k.italic);
  CHECK(k.elements.at("value").size == 44);
  CHECK(k.elements.at("value").weight == 600);
  CHECK(k.elements.at("icon").size == 30);
  CHECK_FALSE(k.elements.at("icon").weight.has_value());
  CHECK(r.config->strip[0].slot->font == "Cascadia Code");
  json out = ld::to_json(*r.config);
  CHECK(out["appearance"]["font"] == "Consolas");
  CHECK(out["appearance"]["italic"] == true);
  CHECK(out["keys"][0]["font"] == "Bahnschrift");
  CHECK(out["keys"][0]["italic"] == false);
  CHECK(out["keys"][0]["elements"] == j["keys"][0]["elements"]);
  CHECK(out["strip"][0]["font"] == "Cascadia Code");
  CHECK(out["strip"][0]["elements"] == j["strip"][0]["elements"]);
  auto again = ld::parse_config(out, kLookup);
  REQUIRE(again.config);
  CHECK(ld::to_json(*again.config) == out);
}

TEST_CASE("a font missing on this PC is kept as written, never an error (configs move between PCs)") {
  json j = valid_json();
  j["appearance"] = {{"font", "No Such Font 4242"}, {"italic", false}};
  j["keys"][0]["font"] = "Another Missing Font";
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.errors.empty());
  json out = ld::to_json(*r.config);
  CHECK(out["appearance"]["font"] == "No Such Font 4242");
  CHECK(out["keys"][0]["font"] == "Another Missing Font");
}

TEST_CASE("null or empty slot font and italic mean inherit") {
  json j = valid_json();
  j["keys"][0]["font"] = "";
  j["keys"][0]["italic"] = nullptr;
  j["keys"][0]["elements"] = nullptr;
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.config->keys[0]->font.empty());
  CHECK_FALSE(r.config->keys[0]->italic.has_value());
  CHECK_FALSE(ld::to_json(*r.config)["keys"][0].contains("font"));
}

TEST_CASE("appearance values of the wrong type or out of bounds are reported with their path") {
  json j = valid_json();
  j["appearance"] = {{"font", 12}, {"italic", "yes"}};
  j["keys"][0]["font"] = true;
  j["keys"][0]["italic"] = 1;
  j["keys"][0]["elements"] = {{"value", {{"size", 81}, {"weight", 500}}}, {"icon", 3}};
  j["strip"][0]["elements"] = "big";
  auto r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  auto has = [&](const std::string& path) {
    return std::any_of(r.errors.begin(), r.errors.end(), [&](const ld::ConfigError& e) { return e.path == path; });
  };
  CHECK(has("appearance.font"));
  CHECK(has("appearance.italic"));
  CHECK(has("keys[0].font"));
  CHECK(has("keys[0].italic"));
  CHECK(has("keys[0].elements.value.size"));
  CHECK(has("keys[0].elements.value.weight"));
  CHECK(has("keys[0].elements.icon"));
  CHECK(has("strip[0].elements"));

  j = valid_json();
  j["appearance"] = {{"font", ""}};
  CHECK_FALSE(ld::parse_config(j, kLookup).config);  // the global font is never empty
  j["appearance"] = {{"font", std::string(101, 'x')}};
  CHECK_FALSE(ld::parse_config(j, kLookup).config);
  j["appearance"] = "Segoe UI";
  auto bad = ld::parse_config(j, kLookup);
  CHECK_FALSE(bad.config);
  REQUIRE(bad.errors.size() == 1);
  CHECK(bad.errors[0].path == "appearance");
}

TEST_CASE("unknown elements are dropped like unknown settings; icons take no weight") {
  json j = valid_json();
  j["keys"][0]["elements"] = {{"gone", {{"size", 20}}}, {"icon", {{"size", 30}, {"weight", 700}}}, {"value", json::object()}};
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  const auto& els = r.config->keys[0]->elements;
  CHECK(els.size() == 1);
  CHECK(els.at("icon").size == 30);
  CHECK_FALSE(els.at("icon").weight.has_value());
  CHECK(ld::to_json(*r.config)["keys"][0]["elements"] == json{{"icon", {{"size", 30}}}});
}

TEST_CASE("unknown keys inside appearance are kept") {
  json doc = valid_json();
  doc["appearance"] = {{"font", "Consolas"}, {"italic", false}, {"future", 1}};
  json extra = ld::unknown_keys(doc);
  CHECK(extra == json{{"appearance", {{"future", 1}}}});
  auto c = ld::parse_config(doc, kLookup).config.value();
  json merged = ld::with_unknown_keys(ld::to_json(c), extra);
  CHECK(merged["appearance"]["future"] == 1);
  CHECK(merged["appearance"]["font"] == "Consolas");
}

TEST_CASE("element visibility, row, alignment and colour round-trip") {
  json j = valid_json();
  j["keys"][0]["elements"] = {
      {"value", {{"visible", false}, {"row", "bottom"}, {"align", "right"}, {"color", "gradient"}}},
      {"graph", {{"row", "middle"}, {"color", "#123456"}}},
      {"icon", {{"align", "left"}, {"color", "state"}}}};
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  const auto& els = r.config->keys[0]->elements;
  CHECK(els.at("value").visible == false);
  CHECK(els.at("value").row == ld::Row::Bottom);
  CHECK(els.at("value").align == ld::HAlign::Right);
  CHECK(els.at("value").color == ld::ColorChoice{ld::ColorMode::Gradient, {}});
  CHECK(els.at("graph").color == ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{0x12, 0x34, 0x56}});
  CHECK(els.at("icon").color == ld::ColorChoice{ld::ColorMode::State, {}});
  json out = ld::to_json(*r.config);
  CHECK(out["keys"][0]["elements"] == j["keys"][0]["elements"]);
  auto again = ld::parse_config(out, kLookup);
  REQUIRE(again.config);
  CHECK(again.config->keys[0]->elements == els);
}

TEST_CASE("a graph outside the middle and bottom rows, or line1 on a three-row key, is refused with its path") {
  json j = valid_json();
  j["keys"][0]["elements"] = {{"graph", {{"row", "top"}}}, {"value", {{"row", "line1"}}}};
  auto r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  auto has = [&](const std::string& path) {
    return std::any_of(r.errors.begin(), r.errors.end(), [&](const ld::ConfigError& e) { return e.path == path; });
  };
  CHECK(has("keys[0].elements.graph.row"));
  CHECK(has("keys[0].elements.value.row"));
  json pair = valid_json();
  pair["keys"][1] = {{"module", "pairmod"}, {"elements", {{"line1", {{"row", "line2"}}}, {"line2", {{"row", "top"}}}}}};
  CHECK(ld::parse_config(pair, kLookup).config);  // two-line keys take every text row
}

TEST_CASE("unknown row, alignment, colour or visibility values are refused with their path") {
  json j = valid_json();
  j["keys"][0]["elements"] = {{"value", {{"row", "side"}, {"align", "middle"}, {"color", "red"}, {"visible", "yes"}}},
                              {"icon", {{"color", "gradient"}}}};
  auto r = ld::parse_config(j, kLookup);
  CHECK_FALSE(r.config);
  auto has = [&](const std::string& path) {
    return std::any_of(r.errors.begin(), r.errors.end(), [&](const ld::ConfigError& e) { return e.path == path; });
  };
  CHECK(has("keys[0].elements.value.row"));
  CHECK(has("keys[0].elements.value.align"));
  CHECK(has("keys[0].elements.value.color"));
  CHECK(has("keys[0].elements.value.visible"));
  CHECK(has("keys[0].elements.icon.color"));  // no gradient on this element
}

TEST_CASE("the state palette defaults, round-trips and reports a bad colour with its path") {
  json j = valid_json();
  j["appearance"] = {{"font", "Segoe UI"}, {"italic", false}, {"states", {{"stale", "#202020"}}}};
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.config->appearance.states.stale == ld::Rgb{0x20, 0x20, 0x20});
  CHECK(r.config->appearance.states.warning == ld::StatePalette{}.warning);
  json out = ld::to_json(*r.config);
  CHECK(out["appearance"]["states"] == json{{"stale", "#202020"}});  // only the changed field
  auto back = ld::parse_config(out, kLookup);
  REQUIRE(back.config);
  CHECK(back.config->appearance.states == r.config->appearance.states);
  j["appearance"]["states"] = {{"idle", "grey"}};
  auto bad = ld::parse_config(j, kLookup);
  REQUIRE_FALSE(bad.config);
  CHECK(bad.errors[0].path == "appearance.states.idle");
  j["appearance"]["states"] = 3;
  bad = ld::parse_config(j, kLookup);
  REQUIRE_FALSE(bad.config);
  CHECK(bad.errors[0].path == "appearance.states");
}

TEST_CASE("unknown keys inside appearance.states survive a save") {
  json doc = valid_json();
  doc["appearance"] = {{"font", "Segoe UI"}, {"italic", false}, {"states", {{"stale", "#202020"}, {"future", "#010203"}}}};
  json extra = ld::unknown_keys(doc);
  CHECK(extra == json{{"appearance", {{"states", {{"future", "#010203"}}}}}});
  auto c = ld::parse_config(doc, kLookup).config.value();
  json merged = ld::with_unknown_keys(ld::to_json(c), extra);
  CHECK(merged["appearance"]["states"]["future"] == "#010203");
  CHECK(merged["appearance"]["states"]["stale"] == "#202020");
  json plain = valid_json();  // no states of its own: the unknown one is still kept
  plain["appearance"] = {{"states", {{"future", 1}}}};
  merged = ld::with_unknown_keys(ld::to_json(ld::parse_config(plain, kLookup).config.value()), ld::unknown_keys(plain));
  CHECK(merged["appearance"]["states"] == json{{"future", 1}});
}

TEST_CASE("text roles: read, written back with only the fields set, round-trip") {
  json j = valid_json();
  j["appearance"] = {{"font", "Segoe UI"},
                     {"italic", false},
                     {"roles", {{"label", {{"size", 20}, {"weight", 700}, {"font", "Bahnschrift"}}},
                                {"value", {{"size", 40}}},
                                {"symbol", json::object()},
                                {"long_value", nullptr},
                                {"secondary", {{"size", nullptr}, {"weight", nullptr}, {"font", ""}}}}}};
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  const ld::RoleOverrides& roles = r.config->appearance.roles;
  REQUIRE(roles.size() == 2);  // an empty, null or all-null role is not set
  CHECK(roles.at(ld::TextRole::Label) == ld::RoleOverride{20, 700, "Bahnschrift"});
  CHECK(roles.at(ld::TextRole::Value) == ld::RoleOverride{40, std::nullopt, ""});
  json out = ld::to_json(*r.config);
  CHECK(out["appearance"]["roles"] == json{{"label", {{"font", "Bahnschrift"}, {"size", 20}, {"weight", 700}}},
                                           {"value", {{"size", 40}}}});
  auto back = ld::parse_config(out, kLookup);
  REQUIRE(back.config);
  CHECK(back.config->appearance == r.config->appearance);
}

TEST_CASE("no role set: appearance.roles is not written, and a config without it has none") {
  json j = valid_json();
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.config->appearance.roles.empty());
  CHECK_FALSE(ld::to_json(*r.config)["appearance"].contains("roles"));
  j["appearance"] = {{"roles", json::object()}};
  r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK_FALSE(ld::to_json(*r.config)["appearance"].contains("roles"));
  j["appearance"] = {{"roles", nullptr}};
  CHECK(ld::parse_config(j, kLookup).config);
}

TEST_CASE("text roles: an unknown role or field, a size outside 8-60, a bad weight or font are refused with their path") {
  auto first_error = [](json roles) {
    json j = valid_json();
    j["appearance"] = {{"roles", std::move(roles)}};
    auto r = ld::parse_config(j, kLookup);
    REQUIRE_FALSE(r.config);
    return r.errors.at(0).path;
  };
  CHECK(first_error({{"title", {{"size", 20}}}}) == "appearance.roles.title");
  CHECK(first_error({{"value", {{"size", 7}}}}) == "appearance.roles.value.size");
  CHECK(first_error({{"value", {{"size", 61}}}}) == "appearance.roles.value.size");
  CHECK(first_error({{"value", {{"size", 20.5}}}}) == "appearance.roles.value.size");
  CHECK(first_error({{"label", {{"weight", 500}}}}) == "appearance.roles.label.weight");
  CHECK(first_error({{"label", {{"weight", "bold"}}}}) == "appearance.roles.label.weight");
  CHECK(first_error({{"symbol", {{"color", "#ffffff"}}}}) == "appearance.roles.symbol.color");
  CHECK(first_error({{"symbol", {{"font", 3}}}}) == "appearance.roles.symbol.font");
  CHECK(first_error({{"symbol", {{"font", std::string(ld::kMaxFontName + 1, 'x')}}}}) == "appearance.roles.symbol.font");
  CHECK(first_error({{"two_line", 5}}) == "appearance.roles.two_line");
  CHECK(first_error(json::array()) == "appearance.roles");
  json j = valid_json();  // the bounds themselves are accepted
  j["appearance"] = {{"roles", {{"value", {{"size", 8}}}, {"label", {{"size", 60}, {"weight", 400}}}}}};
  CHECK(ld::parse_config(j, kLookup).config);
}

TEST_CASE("appearance.roles belongs to the parser: never kept aside as unknown, so a removed role stays removed") {
  json doc = valid_json();
  doc["appearance"] = {{"font", "Segoe UI"}, {"italic", false}, {"roles", {{"value", {{"size", 40}}}}}};
  CHECK(ld::unknown_keys(doc) == json::object());
  auto c = ld::parse_config(doc, kLookup).config.value();
  c.appearance.roles.clear();
  CHECK_FALSE(ld::with_unknown_keys(ld::to_json(c), ld::unknown_keys(doc))["appearance"].contains("roles"));
}

TEST_CASE("keys, knobs and ring colours are lists of any length; missing ring colours have defaults") {
  json j = valid_json();
  j["keys"].erase(0);  // 9 keys
  j["knobs"] = {nullptr, nullptr};
  j["device"]["ring_colors"] = {"#010203"};
  auto r = ld::parse_config(j, kLookup);
  REQUIRE(r.config);
  CHECK(r.config->keys.size() == 9);
  CHECK(r.config->knobs.size() == 2);
  REQUIRE(r.config->ring_colors.size() == 1);
  CHECK(ld::ring_color(*r.config, 0) == ld::Rgb{1, 2, 3});
  CHECK(ld::ring_color(*r.config, 1) == ld::Rgb{0x00, 0x21, 0x32});  // the default of knob 1
  CHECK(ld::ring_color(*r.config, 5) == ld::Rgb{0x20, 0x20, 0x20});  // dark grey beyond the fourth
  CHECK(ld::ring_color(*r.config, -1) == ld::Rgb{0x20, 0x20, 0x20});
  CHECK_FALSE(ld::slot_at(r.config->keys, 40));
  CHECK_FALSE(ld::slot_at(r.config->keys, -1));
}

TEST_CASE("fit_to_shape fills a smaller config with empty slots and default rings, never cuts a larger one") {
  auto c = ld::parse_config(valid_json(), kLookup).config.value();
  const ld::ConfigShape bigger{"test-deck", "Test Deck", 15, 6, 800};
  ld::fit_to_shape(c, bigger);
  CHECK(c.keys.size() == 15);
  REQUIRE(c.keys[0]);
  CHECK(c.keys[0]->module == "keymod");  // kept in place
  CHECK_FALSE(c.keys[14]);
  CHECK(c.knobs.size() == 6);
  REQUIRE(c.ring_colors.size() == 6);
  CHECK(c.ring_colors[3] == ld::Rgb{0x3d, 0x1f, 0x00});
  CHECK(c.ring_colors[5] == ld::Rgb{0x20, 0x20, 0x20});
  CHECK(c.model == "test-deck");
  ld::fit_to_shape(c, ld::ConfigShape{});  // back to the N4 Pro: nothing is cut
  CHECK(c.keys.size() == 15);
  CHECK(c.knobs.size() == 6);
  CHECK(c.model == ld::kDefaultDevice);
}

TEST_CASE("device.model: absent means the N4 Pro and is never written for it; another id is read and written") {
  auto c = ld::parse_config(valid_json(), kLookup).config.value();
  CHECK(c.model == ld::kDefaultDevice);
  CHECK_FALSE(ld::to_json(c)["device"].contains("model"));
  json j = valid_json();
  j["device"]["model"] = "test-deck";
  c = ld::parse_config(j, kLookup).config.value();
  CHECK(c.model == "test-deck");
  CHECK(ld::to_json(c)["device"]["model"] == "test-deck");
  CHECK_FALSE(ld::unknown_keys(j).contains("device"));  // model is a known key
  for (const json& bad : {json(""), json(42), json(std::string(65, 'x'))}) {
    j["device"]["model"] = bad;
    const auto r = ld::parse_config(j, kLookup);
    CHECK_FALSE(r.config);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].path == "device.model");
  }
}

TEST_CASE("the strip is checked against the device's strip width") {
  json j = valid_json();
  j["strip"] = {{{"width", 300}, {"module", nullptr}}, {{"width", 300}, {"module", nullptr}}};
  CHECK_FALSE(ld::parse_config(j, kLookup).config);  // 600 != 800
  ld::ConfigShape six_hundred;
  six_hundred.strip_width = 600;
  CHECK(ld::parse_config(j, kLookup, six_hundred).config);
  j["strip"][0]["width"] = 700;
  CHECK_FALSE(ld::parse_config(j, kLookup, six_hundred).config);  // wider than the strip
}

TEST_CASE("slots_beyond_device: a module outside the device must be the one already kept; empty is fine") {
  auto current = ld::parse_config(valid_json(), kLookup).config.value();
  current.keys.resize(12);
  current.keys[11] = ld::SlotConfig{"keymod", json{{"color", "#ffffff"}}};
  ld::Config next = current;
  CHECK(ld::slots_beyond_device(next, current, ld::ConfigShape{}).empty());  // sent back as kept
  next.keys[10] = ld::SlotConfig{"keymod", json{{"color", "#ffffff"}}};    // a new module beyond key 9
  next.keys[11].reset();                                                    // cleared: fine
  next.knobs.resize(5);
  next.knobs[4] = ld::SlotConfig{"keymod", json::object()};
  const auto errors = ld::slots_beyond_device(next, current, ld::ConfigShape{});
  REQUIRE(errors.size() == 2);
  CHECK(errors[0].path == "keys[10]");
  CHECK(errors[0].message == "the MiraBox N4 Pro E has 10 keys");
  CHECK(errors[1].path == "knobs[4]");
  CHECK(errors[1].message == "the MiraBox N4 Pro E has 4 knobs");
}

namespace {
std::vector<ld::ZoneConfig> zones(std::initializer_list<int> widths) {
  std::vector<ld::ZoneConfig> out;
  for (int w : widths) out.push_back(ld::ZoneConfig{w, std::nullopt});
  return out;
}
std::vector<int> widths(const ld::Config& c) {
  std::vector<int> out;
  for (const auto& z : c.strip) out.push_back(z.width);
  return out;
}
}  // namespace

TEST_CASE("fit_to_shape scales the strip zones to the device's strip, exactly, and only when needed") {
  ld::ConfigShape s600;
  s600.strip_width = 600;
  ld::Config c;
  c.strip = zones({430, 370});
  ld::fit_to_shape(c, s600);
  CHECK(widths(c) == std::vector<int>{322, 278});  // proportional, the last zone takes the rounding
  ld::fit_to_shape(c, s600);
  CHECK(widths(c) == std::vector<int>{322, 278});  // idempotent
  c.strip = zones({430, 370});
  ld::fit_to_shape(c, ld::ConfigShape{});
  CHECK(widths(c) == std::vector<int>{430, 370});  // already 800 on the N4 Pro: untouched
  ld::ConfigShape s300;
  s300.strip_width = 300;
  c.strip = zones({680, 60, 60});
  ld::fit_to_shape(c, s300);
  CHECK(widths(c) == std::vector<int>{180, 60, 60});  // never under kMinZoneWidth
  ld::ConfigShape s400;
  s400.strip_width = 400;
  c.strip = zones({100, 100, 100, 100, 100, 100, 100, 100});
  ld::fit_to_shape(c, s400);
  CHECK(widths(c) == std::vector<int>{66, 66, 66, 66, 66, 70});  // six zones of 60 px at most fit in 400
  ld::ConfigShape no_strip{"test-deck", "Test Deck", 15, 2, 800, false};
  c.strip = zones({300, 300});
  ld::fit_to_shape(c, no_strip);
  CHECK(widths(c) == std::vector<int>{300, 300});  // no strip: the section is kept as is
}

TEST_CASE("the strip of another device's config is read and left to fit_to_shape; this device's must add up") {
  ld::ConfigShape s600{"strip-600", "Strip 600", 10, 4, 600};
  json j = valid_json();  // no model: an N4 Pro config, 430 + 370
  auto r = ld::parse_config(j, kLookup, s600);
  REQUIRE(r.config);
  ld::fit_to_shape(*r.config, s600);
  CHECK(widths(*r.config) == std::vector<int>{322, 278});
  j["device"]["model"] = "strip-600";  // written on this device: must add up to 600
  CHECK_FALSE(ld::parse_config(j, kLookup, s600).config);
  j["strip"] = {{{"width", 4000}, {"module", nullptr}}, {{"width", 200}, {"module", nullptr}}};
  j["device"]["model"] = "elsewhere";
  CHECK_FALSE(ld::parse_config(j, kLookup, s600).config);  // more than any strip
  ld::ConfigShape no_strip{"test-deck", "Test Deck", 15, 2, 800, false};
  j = valid_json();
  j["device"]["model"] = "test-deck";
  j["strip"] = {{{"width", 300}, {"module", nullptr}}, {{"width", 300}, {"module", nullptr}}};
  CHECK(ld::parse_config(j, kLookup, no_strip).config);  // kept as is, never shown
}

TEST_CASE("slots_beyond_device says knob, not knobs, for a device with one") {
  auto current = ld::parse_config(valid_json(), kLookup).config.value();
  ld::Config next = current;
  next.knobs[1] = ld::SlotConfig{"keymod", json::object()};
  const ld::ConfigShape one{"one", "One Knob", 10, 1, 800};
  const auto errors = ld::slots_beyond_device(next, current, one);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].message == "the One Knob has 1 knob");
}

TEST_CASE("configured_device reads device.model without validating the rest of the file") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_cfg_model";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto write = [&](const std::string& text) { std::ofstream(dir / "config.json", std::ios::binary) << text; };
  CHECK(ld::configured_device(dir / "missing.json").empty());
  write(R"({"version": 1, "device": {"model": "test-deck"}, "keys": "broken"})");
  CHECK(ld::configured_device(dir / "config.json") == "test-deck");
  write(R"({"version": 1, "device": {"brightness": 40}})");
  CHECK(ld::configured_device(dir / "config.json").empty());
  write("{ not json");
  CHECK(ld::configured_device(dir / "config.json").empty());
  write(R"({"device": {"model": 7}})");
  CHECK(ld::configured_device(dir / "config.json").empty());
  write(R"({"device": {"model": ")" + std::string(65, 'x') + R"("}})");
  CHECK(ld::configured_device(dir / "config.json").empty());
  fs::remove_all(dir);
}
