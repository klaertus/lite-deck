#include "modules/schema.h"
#include <algorithm>
#include "core/types.h"

namespace ld {
using nlohmann::json;

namespace {
// The values of enum options: plain values, or the "value" of {"value", "label"}.
json option_values(const json& options) {
  json out = json::array();
  for (const json& o : options) out.push_back(o.is_object() && o.contains("value") ? o["value"] : o);
  return out;
}

// A show_if (schema.h): {"field": another key, "equals": v | "in": [..], ["and": condition]}, or a
// non-empty list of such conditions, any one of which shows the field.
bool valid_condition(const json& c, const json& fields, const std::string& key, int depth) {
  if (depth > 4) return false;
  if (c.is_array())
    return !c.empty() && std::all_of(c.begin(), c.end(), [&](const json& x) {
      return x.is_object() && valid_condition(x, fields, key, depth + 1);
    });
  if (!c.is_object() || !c.contains("field") || !c["field"].is_string()) return false;
  const std::string field = c["field"].get<std::string>();
  const bool named = field != key && std::any_of(fields.begin(), fields.end(), [&](const json& g) {
                       return g.is_object() && g.value("key", std::string()) == field;
                     });
  if (!named || !(c.contains("equals") || (c.contains("in") && c["in"].is_array()))) return false;
  return !c.contains("and") || (c["and"].is_object() && valid_condition(c["and"], fields, key, depth + 1));
}

// Checks one value against its field; returns an error message or nullopt.
std::optional<std::string> check_value(const json& f, const std::string& key, const std::string& type, const json& v) {
  if (type == "int") {
    if (!v.is_number_integer()) return key + ": integer expected";
    double x = v.get<double>();
    if (f.contains("min") && f["min"].is_number() && x < f["min"].get<double>()) return key + ": value too small (min " + f["min"].dump() + ")";
    if (f.contains("max") && f["max"].is_number() && x > f["max"].get<double>()) return key + ": value too large (max " + f["max"].dump() + ")";
  } else if (type == "number") {
    if (!v.is_number()) return key + ": number expected";
    double x = v.get<double>();
    if (f.contains("min") && f["min"].is_number() && x < f["min"].get<double>()) return key + ": value too small (min " + f["min"].dump() + ")";
    if (f.contains("max") && f["max"].is_number() && x > f["max"].get<double>()) return key + ": value too large (max " + f["max"].dump() + ")";
  } else if (type == "bool") {
    if (!v.is_boolean()) return key + ": true/false expected";
  } else if (type == "string" || type == "secret") {  // a secret is a string the page masks
    if (!v.is_string()) return key + ": string expected";
  } else if (type == "color") {
    if (!v.is_string() || !parse_color(v.get<std::string>())) return key + ": color #rrggbb expected";
  } else if (type == "enum") {
    if (!f.contains("options") || !f["options"].is_array()) return std::string("options missing");
    const json opts = option_values(f["options"]);
    if (!v.is_string() || std::find(opts.begin(), opts.end(), v) == opts.end())
      return key + ": value not allowed (" + opts.dump() + ")";
  } else {
    return "unknown type " + type;
  }
  return std::nullopt;
}
}  // namespace

std::optional<std::string> apply_schema(const json& schema, const json& in, json& out) {
  out = json::object();
  if (!schema.is_object() || !schema.contains("fields") || !schema["fields"].is_array())
    return std::string("invalid schema: \"fields\" missing");
  for (const auto& f : schema["fields"]) {
    if (!f.is_object() || !f.contains("key") || !f["key"].is_string() || !f.contains("type") || !f["type"].is_string())
      return std::string("invalid schema: field without key/type");
    const std::string key = f["key"].get<std::string>();
    const std::string type = f["type"].get<std::string>();
    if (!f.contains("default")) return "invalid schema: " + key + " without a default value";
    if (auto e = check_value(f, key, type, f["default"])) return "invalid schema: " + *e;
    if (f.contains("show_if") && !valid_condition(f["show_if"], schema["fields"], key, 0))
      return "invalid schema: " + key + ": show_if needs another field and equals or in";
  }
  if (!in.is_object()) return std::string("settings must be an object");
  for (const auto& f : schema["fields"]) {
    const std::string key = f["key"].get<std::string>();
    if (!in.contains(key)) {
      out[key] = f["default"];
      continue;
    }
    if (auto e = check_value(f, key, f["type"].get<std::string>(), in[key])) return e;
    out[key] = in[key];
  }
  return std::nullopt;
}

}  // namespace ld
