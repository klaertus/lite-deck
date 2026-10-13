#include <doctest/doctest.h>
#include <set>
#include "modules/module.h"
#include "modules/schema.h"

using nlohmann::json;

static const json kSchema = json::parse(R"({"fields":[
  {"key":"color","type":"color","default":"#ffffff","label":"Color"},
  {"key":"step","type":"int","default":2,"min":1,"max":10,"label":"Step"},
  {"key":"ratio","type":"number","default":0.5,"min":0,"max":1,"label":"Ratio"},
  {"key":"seconds","type":"bool","default":true,"label":"Secondes"},
  {"key":"city","type":"string","default":"Brussels","label":"City"},
  {"key":"mode","type":"enum","default":"vram","options":["vram","temp"],"label":"Mode"}
]})");

TEST_CASE("apply_schema fills defaults and drops unknown keys") {
  json out;
  auto err = ld::apply_schema(kSchema, json{{"step", 5}, {"junk", 1}}, out);
  CHECK_FALSE(err);
  CHECK(out["step"] == 5);
  CHECK(out["color"] == "#ffffff");
  CHECK(out["mode"] == "vram");
  CHECK_FALSE(out.contains("junk"));
}

TEST_CASE("apply_schema rejects wrong types and out-of-range values") {
  json out;
  CHECK(ld::apply_schema(kSchema, json{{"step", 11}}, out));
  CHECK(ld::apply_schema(kSchema, json{{"step", "2"}}, out));
  CHECK(ld::apply_schema(kSchema, json{{"ratio", 2.0}}, out));
  CHECK(ld::apply_schema(kSchema, json{{"seconds", 1}}, out));
  CHECK(ld::apply_schema(kSchema, json{{"color", "red"}}, out));
  CHECK(ld::apply_schema(kSchema, json{{"mode", "fan"}}, out));
  CHECK(ld::apply_schema(kSchema, json::array(), out));
  auto e = ld::apply_schema(kSchema, json{{"step", 0}}, out);
  REQUIRE(e);
  CHECK(e->find("step") != std::string::npos);
}

TEST_CASE("ModuleInfo::allows checks slot kinds") {
  ld::ModuleInfo info;
  info.slots = {ld::SlotKind::Key};
  CHECK(info.allows(ld::SlotKind::Key));
  CHECK_FALSE(info.allows(ld::SlotKind::Knob));
}

TEST_CASE("apply_schema reports a broken schema instead of throwing") {
  json out;
  auto bad_type = json::parse(R"({"fields":[{"key":"a","type":"weird","default":1,"label":"a"}]})");
  auto no_default = json::parse(R"({"fields":[{"key":"a","type":"int","label":"a"}]})");
  auto bad_default = json::parse(R"({"fields":[{"key":"m","type":"enum","options":["x","y"],"default":"z","label":"m"}]})");
  auto no_key = json::parse(R"({"fields":[{"type":"int","default":1}]})");
  for (const auto& s : {bad_type, no_default, bad_default, no_key}) {
    std::optional<std::string> e;
    CHECK_NOTHROW(e = ld::apply_schema(s, json::object(), out));
    REQUIRE(e);
    CHECK(e->find("invalid schema") != std::string::npos);
  }
}

TEST_CASE("every registered module has a valid schema, a factory and a slot kind") {
  std::set<std::string> ids;
  for (const auto& m : ld::module_registry()) {
    INFO(m.id);
    json out;
    CHECK_FALSE(ld::apply_schema(m.settings_schema, json::object(), out));
    CHECK(static_cast<bool>(m.create));
    CHECK_FALSE(m.slots.empty());
    CHECK(ids.insert(m.id).second);
  }
}

TEST_CASE("apply_schema takes labelled enum options ({value, label}) as their values") {
  const json schema = json::parse(R"({"fields":[{"key":"tap","type":"enum","default":"none","label":"On tap",
    "options":[{"value":"none","label":"Nothing"},{"value":"open","label":"Open a link"}]}]})");
  json out;
  CHECK_FALSE(ld::apply_schema(schema, json{{"tap", "open"}}, out));
  CHECK(out["tap"] == "open");
  auto e = ld::apply_schema(schema, json{{"tap", "Nothing"}}, out);  // a label is not a value
  REQUIRE(e);
  CHECK(*e == "tap: value not allowed ([\"none\",\"open\"])");
}

TEST_CASE("apply_schema checks show_if: another field, and equals or in") {
  json out;
  const auto with = [](const char* show_if) {
    return json::parse(std::string(R"({"fields":[{"key":"kind","type":"enum","default":"a","options":["a","b"],"label":"Kind"},
      {"key":"path","type":"string","default":"","label":"Path","show_if":)") + show_if + "}]}");
  };
  CHECK_FALSE(ld::apply_schema(with(R"({"field":"kind","equals":"a"})"), json::object(), out));
  CHECK_FALSE(ld::apply_schema(with(R"({"field":"kind","in":["a","b"]})"), json::object(), out));
  for (const char* bad : {R"({"field":"nope","equals":"a"})", R"({"field":"kind"})", R"({"field":"path","equals":""})",
                          R"("kind")", R"({"field":"kind","in":"a"})"}) {
    INFO(bad);
    auto e = ld::apply_schema(with(bad), json::object(), out);
    REQUIRE(e);
    CHECK(e->find("invalid schema") != std::string::npos);
  }
}

TEST_CASE("apply_schema checks show_if lists (any one) and and-conditions") {
  json out;
  const auto with = [](const char* show_if) {
    return json::parse(std::string(R"({"fields":[{"key":"mode","type":"enum","default":"a","options":["a","b"],"label":"Mode"},
      {"key":"on","type":"bool","default":false,"label":"On"},
      {"key":"level","type":"number","default":1,"label":"Level","show_if":)") + show_if + "}]}");
  };
  CHECK_FALSE(ld::apply_schema(with(R"([{"field":"mode","equals":"a"},{"field":"mode","equals":"b","and":{"field":"on","equals":true}}])"),
                               json::object(), out));
  for (const char* bad : {R"([])", R"([{"field":"nope","equals":"a"}])", R"([[{"field":"mode","equals":"a"}]])",
                          R"({"field":"mode","equals":"a","and":{"field":"level","equals":1}})",
                          R"({"field":"mode","equals":"a","and":[{"field":"on","equals":true}]})"}) {
    INFO(bad);
    auto e = ld::apply_schema(with(bad), json::object(), out);
    REQUIRE(e);
    CHECK(e->find("invalid schema") != std::string::npos);
  }
}
