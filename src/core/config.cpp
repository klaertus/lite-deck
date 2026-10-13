// src/core/config.cpp
#include "core/config.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include "core/config_migration.h"
#include "core/files.h"
#include "core/log.h"
#include "core/strings.h"
#include "modules/schema.h"
#include "render/look_json.h"

namespace ld {
using nlohmann::json;

namespace {

constexpr std::pair<const char*, Rgb StatePalette::*> kStateColors[] = {
    {"warning", &StatePalette::warning}, {"critical", &StatePalette::critical},
    {"idle", &StatePalette::idle},       {"stale", &StatePalette::stale}};

constexpr std::array<Rgb, 4> kDefaultRings{Rgb{0x3e, 0x00, 0x00}, Rgb{0x00, 0x21, 0x32}, Rgb{0x1b, 0x35, 0x00},
                                           Rgb{0x3d, 0x1f, 0x00}};
constexpr Rgb kExtraRing{0x20, 0x20, 0x20};

bool has(const json& obj, const char* key) { return obj.is_object() && obj.contains(key) && !obj.at(key).is_null(); }

struct Parser {
  const ModuleLookup& lookup;
  std::vector<ConfigError> errors;

  void err(std::string path, std::string msg) { errors.push_back({std::move(path), std::move(msg)}); }

  int int_in(const json& obj, const char* key, int def, int lo, int hi, const std::string& path) {
    if (!obj.is_object() || !obj.contains(key)) return def;
    const json& v = obj.at(key);
    std::string p = path + "." + key;
    if (!v.is_number_integer()) { err(p, "must be an integer"); return def; }
    long long x = v.is_number_unsigned() ? (v.get<std::uint64_t>() > 1000000000000ULL ? 1000000000000LL : static_cast<long long>(v.get<std::uint64_t>())) : v.get<long long>();
    if (x < lo || x > hi) { err(p, std::format("must be between {} and {}", lo, hi)); return def; }
    return static_cast<int>(x);
  }

  // A font family name: a string of at most kMaxFontName bytes. Whether it is installed is the
  // Deck's business (a config may come from another PC).
  std::optional<std::string> font_name(const json& v, const std::string& path) {
    if (!v.is_string()) { err(path, "must be a font name"); return std::nullopt; }
    std::string s = v.get<std::string>();
    if (s.size() > kMaxFontName) { err(path, std::format("must be at most {} characters", kMaxFontName)); return std::nullopt; }
    return s;
  }

  // appearance.roles: {"<role>": {"size", "weight", "font"}}. Unlike element
  // keys, an unknown role or field is refused: the page sends only known ones, and a typo in a
  // hand-edited file would otherwise do nothing. null (a role or a field) = not set; "" = no font.
  void roles(const json& v, RoleOverrides& dst) {
    if (v.is_null()) return;
    if (!v.is_object()) { err("appearance.roles", "must be an object"); return; }
    for (auto it = v.begin(); it != v.end(); ++it) {
      const std::string p = "appearance.roles." + it.key();
      const std::optional<TextRole> role = role_from_name(it.key());
      if (!role) { err(p, "unknown role (label, value, long_value, two_line, secondary or symbol)"); continue; }
      if (it->is_null()) continue;
      if (!it->is_object()) { err(p, "must be an object of size, weight and font"); continue; }
      RoleOverride o;
      for (auto f = it->begin(); f != it->end(); ++f) {
        const std::string& name = f.key();
        if (name != "size" && name != "weight" && name != "font") {
          err(p + "." + name, "unknown setting (size, weight or font)");
          continue;
        }
        if (f->is_null()) continue;
        if (name == "size") {
          const std::size_t before = errors.size();
          const int size = int_in(*it, "size", 0, kMinRoleSize, kMaxRoleSize, p);
          if (errors.size() == before) o.size = size;
        } else if (name == "weight") {
          if (f->is_number_integer() && (*f == 400 || *f == 600 || *f == 700)) o.weight = f->get<int>();
          else err(p + ".weight", "must be 400, 600 or 700");
        } else if (auto font = font_name(*f, p + ".font")) {
          o.font = *font;
        }
      }
      if (o != RoleOverride{}) dst[*role] = std::move(o);
    }
  }

  // {"<element>": {"size", "weight", "visible", "row", "align", "color"}}. Elements the
  // module does not declare are dropped, like unknown settings; icons take no weight, graphs neither
  // size nor weight. A graph only goes in the middle or bottom row; line1 / line2 exist only on
  // two-line keys; "gradient" only on an element the module gives a metric for.
  void elements(const ModuleInfo& info, const json& v, const std::string& path, ElementOverrides& dst) {
    if (v.is_null()) return;
    if (!v.is_object()) { err(path, "must be an object"); return; }
    const bool two_line = std::any_of(info.elements.begin(), info.elements.end(), [](const ElementSpec& e) {
      return e.row == Row::Line1 || e.row == Row::Line2;
    });
    for (const ElementSpec& e : info.elements) {
      auto it = v.find(e.key);
      if (it == v.end() || it->is_null()) continue;
      const std::string p = path + "." + e.key;
      if (!it->is_object()) { err(p, "must be an object of element settings"); continue; }
      const json& o_json = *it;
      const bool graph = e.kind == ElementKind::Graph;
      ElementOverride o;
      if (!graph && has(o_json, "size")) {
        const std::size_t before = errors.size();
        const int size = int_in(o_json, "size", 0, e.min_size, e.max_size, p);
        if (errors.size() == before) o.size = size;
      }
      if (!graph && !e.icon && has(o_json, "weight")) {
        const json& w = o_json.at("weight");
        if (w.is_number_integer() && (w == 400 || w == 600 || w == 700)) o.weight = w.get<int>();
        else err(p + ".weight", "must be 400, 600 or 700");
      }
      if (has(o_json, "visible")) {
        if (o_json.at("visible").is_boolean()) o.visible = o_json.at("visible").get<bool>();
        else err(p + ".visible", "must be true or false");
      }
      if (has(o_json, "row")) {
        const json& r = o_json.at("row");
        const std::optional<Row> row = r.is_string() ? row_from_name(r.get<std::string>()) : std::nullopt;
        if (!row) err(p + ".row", "must be top, middle, bottom, line1 or line2");
        else if (graph && *row != Row::Middle && *row != Row::Bottom) err(p + ".row", "a graph can only go in the middle or bottom row");
        else if (!two_line && (*row == Row::Line1 || *row == Row::Line2)) err(p + ".row", "line1 and line2 exist only on two-line keys");
        else o.row = *row;
      }
      if (has(o_json, "align")) {
        const json& a = o_json.at("align");
        const std::optional<HAlign> align = a.is_string() ? align_from_name(a.get<std::string>()) : std::nullopt;
        if (align) o.align = *align;
        else err(p + ".align", "must be left, center or right");
      }
      if (has(o_json, "color")) {
        const json& c = o_json.at("color");
        const std::optional<ColorChoice> choice = c.is_string() ? color_choice_from_name(c.get<std::string>()) : std::nullopt;
        if (!choice) err(p + ".color", "must be a color #rrggbb, \"state\" or \"gradient\"");
        else if (choice->mode == ColorMode::Gradient && !e.gradient) err(p + ".color", "this element has no gradient");
        else o.color = *choice;
      }
      if (o != ElementOverride{}) dst[e.key] = o;
    }
  }

  std::optional<SlotConfig> slot(const json& v, SlotKind kind, const std::string& path) {
    if (v.is_null()) return std::nullopt;
    if (!v.is_object() || !v.contains("module")) { err(path, "must be null or an object with \"module\""); return std::nullopt; }
    if (v.at("module").is_null()) return std::nullopt;
    if (!v.at("module").is_string()) { err(path + ".module", "must be a string"); return std::nullopt; }
    std::string id = v.at("module").get<std::string>();
    const ModuleInfo* info = lookup(id);
    if (!info) { err(path + ".module", "unknown module: " + id); return std::nullopt; }
    if (!info->allows(kind)) { err(path + ".module", "module " + id + " cannot be placed here"); return std::nullopt; }
    json out;
    if (auto e = apply_schema(info->settings_schema, v.value("settings", json::object()), out)) {
      err(path + ".settings", *e);
      return std::nullopt;
    }
    if (info->check) {
      std::optional<std::string> e;
      try {
        e = info->check(out);
      } catch (const std::exception& ex) {  // a module's bug refuses its settings, never the parse
        e = std::string("cannot be checked: ") + ex.what();
      }
      if (e) {
        err(path + ".settings", *e);
        return std::nullopt;
      }
    }
    SlotConfig sc{id, out};
    if (v.contains("font") && !v.at("font").is_null())
      if (auto f = font_name(v.at("font"), path + ".font")) sc.font = *f;
    if (v.contains("italic") && !v.at("italic").is_null()) {
      if (v.at("italic").is_boolean()) sc.italic = v.at("italic").get<bool>();
      else err(path + ".italic", "must be true, false or null");
    }
    if (v.contains("elements")) elements(*info, v.at("elements"), path + ".elements", sc.elements);
    return sc;
  }

  void slots(const json& j, const char* key, SlotKind kind, std::size_t max, std::vector<std::optional<SlotConfig>>& dst) {
    if (!j.contains(key)) return;
    const json& arr = j.at(key);
    if (!arr.is_array() || arr.size() > max) { err(key, std::format("must be a list of at most {} items", max)); return; }
    dst.assign(arr.size(), std::nullopt);
    for (std::size_t i = 0; i < arr.size(); ++i) dst[i] = slot(arr[i], kind, std::format("{}[{}]", key, i));
  }
};

}  // namespace

static ParseResult parse_config_impl(const json& j, const ModuleLookup& lookup, const ConfigShape& shape);

ParseResult parse_config(const json& j, const ModuleLookup& lookup, const ConfigShape& shape) {
  try {
    return parse_config_impl(j, lookup, shape);
  } catch (const json::exception& e) {
    return {std::nullopt, {{"", std::string("invalid JSON: ") + e.what()}}};
  }
}

static ParseResult parse_config_impl(const json& j, const ModuleLookup& lookup, const ConfigShape& shape) {
  Parser p{lookup, {}};
  Config c;
  if (!j.is_object()) return {std::nullopt, {{"", "the document must be a JSON object"}}};
  if (!j.contains("version") || !j.at("version").is_number_integer() || j.at("version") != 1)
    p.err("version", "unsupported version (expected 1)");

  json dev = json::object();
  if (j.contains("device")) {
    if (j.at("device").is_object()) dev = j.at("device");
    else p.err("device", "must be an object");
  }
  json web = json::object();
  if (j.contains("web")) {
    if (j.at("web").is_object()) web = j.at("web");
    else p.err("web", "must be an object");
  }
  c.brightness = p.int_in(dev, "brightness", c.brightness, 0, 100, "device");
  c.ring_brightness = p.int_in(dev, "ring_brightness", c.ring_brightness, 0, 100, "device");
  if (dev.contains("model")) {
    const json& m = dev.at("model");
    if (m.is_string() && !m.get<std::string>().empty() && m.get<std::string>().size() <= kMaxModelLength)
      c.model = m.get<std::string>();
    else
      p.err("device.model", std::format("must be a device id of 1 to {} characters", kMaxModelLength));
  }
  if (dev.contains("ring_colors")) {
    const json& rc = dev.at("ring_colors");
    if (!rc.is_array() || rc.size() > kMaxConfigKnobs) {
      p.err("device.ring_colors", std::format("must be a list of at most {} colors", kMaxConfigKnobs));
    } else {
      c.ring_colors.assign(rc.size(), Rgb{});
      for (std::size_t i = 0; i < rc.size(); ++i) {
        auto col = rc[i].is_string() ? parse_color(rc[i].get<std::string>()) : std::nullopt;
        if (col) c.ring_colors[i] = *col;
        else p.err(std::format("device.ring_colors[{}]", i), "color #rrggbb expected");
      }
    }
  }
  c.web_port = p.int_in(web, "port", c.web_port, 1024, 65535, "web");
  if (j.contains("appearance")) {
    const json& a = j.at("appearance");
    if (!a.is_object()) {
      p.err("appearance", "must be an object");
    } else {
      if (a.contains("font")) {
        if (auto f = p.font_name(a.at("font"), "appearance.font")) {
          if (f->empty()) p.err("appearance.font", "must not be empty");
          else c.appearance.font = *f;
        }
      }
      if (a.contains("italic")) {
        if (a.at("italic").is_boolean()) c.appearance.italic = a.at("italic").get<bool>();
        else p.err("appearance.italic", "must be true or false");
      }
      if (a.contains("roles")) p.roles(a.at("roles"), c.appearance.roles);
      if (a.contains("states")) {
        const json& st = a.at("states");
        if (!st.is_object()) {
          p.err("appearance.states", "must be an object");
        } else {
          for (const auto& [name, field] : kStateColors) {
            if (!st.contains(name)) continue;
            const json& v = st.at(name);
            const auto col = v.is_string() ? parse_color(v.get<std::string>()) : std::nullopt;
            if (col) c.appearance.states.*field = *col;
            else p.err(std::string("appearance.states.") + name, "color #rrggbb expected");
          }
        }
      }
    }
  }

  p.slots(j, "keys", SlotKind::Key, kMaxConfigKeys, c.keys);
  p.slots(j, "knobs", SlotKind::Knob, kMaxConfigKnobs, c.knobs);

  if (j.contains("strip")) {
    const json& s = j.at("strip");
    if (!s.is_array() || s.empty() || s.size() > 8) {
      p.err("strip", "must be a list of 1 to 8 zones");
    } else {
      c.strip.clear();
      // Another device's section (or any on a device without a strip) is fitted later, not refused.
      const bool exact = shape.has_strip && c.model == shape.device;
      const int max_width = exact ? std::max(kMinZoneWidth, shape.strip_width) : kMaxConfigStripWidth;  // lo <= hi
      long long total = 0;
      bool zones_ok = true;
      for (std::size_t i = 0; i < s.size(); ++i) {
        std::string path = std::format("strip[{}]", i);
        const json& z = s[i];
        if (!z.is_object() || !z.contains("width") || !z.at("width").is_number_integer()) {
          p.err(path, "invalid zone (integer width required)");
          zones_ok = false;
          continue;
        }
        ZoneConfig zc;
        const json& w = z.at("width");
        long long wv = w.is_number_unsigned() ? (w.get<std::uint64_t>() > 100000ULL ? 100000LL : static_cast<long long>(w.get<std::uint64_t>())) : w.get<long long>();
        if (wv < kMinZoneWidth || wv > max_width) {
          p.err(path + ".width", std::format("must be between {} and {} px", kMinZoneWidth, max_width));
          zones_ok = false;
          wv = std::clamp<long long>(wv, kMinZoneWidth, max_width);
        }
        zc.width = static_cast<int>(wv);
        total += zc.width;
        if (z.contains("module")) zc.slot = p.slot(z, SlotKind::StripZone, path);
        c.strip.push_back(std::move(zc));
      }
      if (zones_ok && exact && total != shape.strip_width)
        p.err("strip", std::format("the widths must add up to {} (currently {})", shape.strip_width, total));
      else if (zones_ok && !exact && total > kMaxConfigStripWidth)
        p.err("strip", std::format("the widths must add up to at most {} (currently {})", kMaxConfigStripWidth, total));
    }
  }

  if (!p.errors.empty()) return {std::nullopt, std::move(p.errors)};
  return {c, {}};
}

// Writes the slot's appearance keys, each only when set: an untouched slot keeps today's format.
static void put_look(json& j, const SlotConfig& s) {
  if (!s.font.empty()) j["font"] = s.font;
  if (s.italic) j["italic"] = *s.italic;
  if (s.elements.empty()) return;
  json els = json::object();
  for (const auto& [key, o] : s.elements) {
    json e = json::object();
    if (o.size) e["size"] = *o.size;
    if (o.weight) e["weight"] = *o.weight;
    if (o.visible) e["visible"] = *o.visible;
    if (o.row) e["row"] = row_name(*o.row);
    if (o.align) e["align"] = align_name(*o.align);
    if (o.color) e["color"] = color_choice_name(*o.color);
    els[key] = std::move(e);
  }
  j["elements"] = std::move(els);
}

static json slot_json(const std::optional<SlotConfig>& s) {
  if (!s) return nullptr;
  json j{{"module", s->module}, {"settings", s->settings}};
  put_look(j, *s);
  return j;
}

json states_to_json(const StatePalette& states) {
  json out = json::object();
  for (const auto& [name, field] : kStateColors) out[name] = format_color(states.*field);
  return out;
}

// Only the colours the user changed: the defaults stay implicit, so they can evolve.
static json changed_states_json(const StatePalette& states) {
  json out = json::object();
  const StatePalette def;
  for (const auto& [name, field] : kStateColors)
    if (!(states.*field == def.*field)) out[name] = format_color(states.*field);
  return out;
}

// Only the roles and fields the user set: an empty role is not written.
static json roles_json(const RoleOverrides& roles) {
  json out = json::object();
  for (const auto& [role, o] : roles) {
    json r = json::object();
    if (o.size) r["size"] = *o.size;
    if (o.weight) r["weight"] = *o.weight;
    if (!o.font.empty()) r["font"] = o.font;
    if (!r.empty()) out[role_name(role)] = std::move(r);
  }
  return out;
}

json to_json(const Config& c) {
  json keys = json::array(), knobs = json::array(), strip = json::array(), rings = json::array();
  for (const auto& k : c.keys) keys.push_back(slot_json(k));
  for (const auto& k : c.knobs) knobs.push_back(slot_json(k));
  for (const auto& r : c.ring_colors) rings.push_back(format_color(r));
  for (const auto& z : c.strip) {
    json zj{{"width", z.width}, {"module", nullptr}};
    if (z.slot) {
      zj["module"] = z.slot->module;
      zj["settings"] = z.slot->settings;
      put_look(zj, *z.slot);
    }
    strip.push_back(zj);
  }
  json appearance{{"font", c.appearance.font}, {"italic", c.appearance.italic}};
  if (json st = changed_states_json(c.appearance.states); !st.empty()) appearance["states"] = std::move(st);
  if (json r = roles_json(c.appearance.roles); !r.empty()) appearance["roles"] = std::move(r);
  json device{{"brightness", c.brightness}, {"ring_brightness", c.ring_brightness}, {"ring_colors", rings}};
  if (c.model != kDefaultDevice) device["model"] = c.model;  // an N4 Pro config keeps its format
  return json{{"version", c.version},
              {"device", std::move(device)},
              {"web", {{"port", c.web_port}}},
              {"appearance", std::move(appearance)},
              {"keys", keys},
              {"knobs", knobs},
              {"strip", strip}};
}

// The settings of `module` with every schema default, then `over`. default_config() bypasses
// apply_schema (the Deck hands settings straight to start()), so it fills them here: a module that
// gains a setting needs no change below. Keys a schema no longer has are dropped.
static json defaults_of(const char* module, const json& over = json::object()) {
  const ModuleInfo* info = find_module(module);
  json out;
  if (!info || apply_schema(info->settings_schema, over, out)) return over;
  return out;
}

Config default_config() {
  Config c;
  // The default layout. Top row: the measures (CPU, GPU, RAM, VRAM) and the stopwatch at the edge;
  // bottom row: time, date, weather, sun and prices. Written on the first run only.
  c.keys[0] = SlotConfig{"cpu_load", defaults_of("cpu_load", {{"show_temperature", true}})};
  c.keys[1] = SlotConfig{"gpu", defaults_of("gpu", {{"mode", "usage"}, {"show_temperature", true}})};
  c.keys[2] = SlotConfig{"ram", defaults_of("ram")};
  c.keys[3] = SlotConfig{"gpu", defaults_of("gpu", {{"mode", "vram"}})};
  c.keys[4] = SlotConfig{"stopwatch", defaults_of("stopwatch")};
  c.keys[5] = SlotConfig{"clock", defaults_of("clock")};
  c.keys[6] = SlotConfig{"date", defaults_of("date")};
  c.keys[7] = SlotConfig{"weather", defaults_of("weather")};
  c.keys[8] = SlotConfig{"sun", defaults_of("sun")};
  c.keys[9] = SlotConfig{"crypto_gold", defaults_of("crypto_gold")};
  c.knobs[0] = SlotConfig{"volume", defaults_of("volume")};
  c.knobs[1] = SlotConfig{"mic", defaults_of("mic")};
  c.knobs[2] = SlotConfig{"media", defaults_of("media")};
  c.knobs[3] = SlotConfig{"seek", defaults_of("seek")};
  c.strip = {ZoneConfig{430, SlotConfig{"now_playing", defaults_of("now_playing")}},
             ZoneConfig{370, SlotConfig{"notifications", defaults_of("notifications")}}};
  return c;
}

Config default_config(const ConfigShape& shape) {
  Config c = default_config();
  fit_to_shape(c, shape);
  return c;
}

LoadResult load_config_file(const std::filesystem::path& file, const ModuleLookup& lookup, const ConfigShape& shape) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return {std::nullopt, {}, true};
  std::ifstream f(file, std::ios::binary);
  if (!f) return {std::nullopt, {{"", "cannot read file"}}, false};
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
  bool too_deep = false;
  json j = parse_json_bounded(text, true, &too_deep);
  if (too_deep) return {std::nullopt, {{"", "JSON nested too deeply"}}, false};
  if (j.is_discarded()) return {std::nullopt, {{"", "invalid JSON"}}, false};
  Migration m = migrate_v1(j);
  auto r = parse_config(m.doc, lookup, shape);
  return {std::move(r.config), std::move(r.errors), false, std::move(m.doc), m.changed};
}

std::string configured_device(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
  const json j = parse_json_bounded(text, true);
  if (!j.is_object()) return {};
  const auto dev = j.find("device");
  if (dev == j.end() || !dev->is_object()) return {};
  const auto m = dev->find("model");
  if (m == dev->end() || !m->is_string()) return {};
  std::string id = m->get<std::string>();
  return id.size() <= kMaxModelLength ? id : std::string();
}

bool save_config_file(const std::filesystem::path& file, const Config& c, const json& extra) {
  const std::string text = with_unknown_keys(to_json(c), extra).dump(2, ' ', false, json::error_handler_t::replace) + "\n";
  return write_file_atomic(file, text);
}

bool backup_once(const std::filesystem::path& file, const std::filesystem::path& backup) {
  std::error_code ec;
  if (std::filesystem::exists(backup, ec)) return true;
  return std::filesystem::copy_file(file, backup, std::filesystem::copy_options::none, ec) && !ec;
}

json unknown_keys(const json& doc) {
  json out = json::object();
  if (!doc.is_object()) return out;
  static const std::set<std::string> top{"version", "device", "web", "appearance", "keys", "knobs", "strip"};
  static const std::set<std::string> device{"model", "brightness", "ring_brightness", "ring_colors"};
  static const std::set<std::string> web{"port"};
  static const std::set<std::string> appearance{"font", "italic", "states", "roles"};
  for (auto it = doc.begin(); it != doc.end(); ++it)
    if (!top.contains(it.key())) out[it.key()] = it.value();
  auto inner = [&](const char* key, const std::set<std::string>& known) {
    auto it = doc.find(key);
    if (it == doc.end() || !it->is_object()) return;
    json extra = json::object();
    for (auto jt = it->begin(); jt != it->end(); ++jt)
      if (!known.contains(jt.key())) extra[jt.key()] = jt.value();
    if (!extra.empty()) out[key] = std::move(extra);
  };
  inner("device", device);
  inner("web", web);
  inner("appearance", appearance);
  auto ap = doc.find("appearance");  // unknown colours inside appearance.states
  if (ap != doc.end() && ap->is_object()) {
    auto st = ap->find("states");
    if (st != ap->end() && st->is_object()) {
      json extra = json::object();
      for (auto jt = st->begin(); jt != st->end(); ++jt) {
        const bool known = std::any_of(std::begin(kStateColors), std::end(kStateColors),
                                       [&](const auto& k) { return jt.key() == k.first; });
        if (!known) extra[jt.key()] = jt.value();
      }
      if (!extra.empty()) out["appearance"]["states"] = std::move(extra);
    }
  }
  return out;
}

json with_unknown_keys(json canonical, const json& extra) {
  if (!extra.is_object() || !canonical.is_object()) return canonical;
  for (auto it = extra.begin(); it != extra.end(); ++it) {
    const std::string& key = it.key();
    if ((key == "device" || key == "web" || key == "appearance") && it->is_object() && canonical.contains(key) && canonical[key].is_object()) {
      for (auto jt = it->begin(); jt != it->end(); ++jt) {
        if (!canonical[key].contains(jt.key())) {
          canonical[key][jt.key()] = jt.value();
        } else if (key == "appearance" && jt.key() == "states" && jt->is_object() && canonical[key][jt.key()].is_object()) {
          for (auto st = jt->begin(); st != jt->end(); ++st)
            if (!canonical[key]["states"].contains(st.key())) canonical[key]["states"][st.key()] = st.value();
        }
      }
    } else if (!canonical.contains(key)) {
      canonical[key] = it.value();
    }
  }
  return canonical;
}

Rgb default_ring_color(int knob) {
  return knob >= 0 && knob < static_cast<int>(kDefaultRings.size()) ? kDefaultRings[static_cast<std::size_t>(knob)] : kExtraRing;
}

std::vector<Rgb> default_ring_colors() { return std::vector<Rgb>(kDefaultRings.begin(), kDefaultRings.end()); }

const std::optional<SlotConfig>& slot_at(const std::vector<std::optional<SlotConfig>>& slots, int i) {
  static const std::optional<SlotConfig> none;
  return i >= 0 && static_cast<std::size_t>(i) < slots.size() ? slots[static_cast<std::size_t>(i)] : none;
}

Rgb ring_color(const Config& c, int knob) {
  return knob >= 0 && static_cast<std::size_t>(knob) < c.ring_colors.size() ? c.ring_colors[static_cast<std::size_t>(knob)]
                                                                             : default_ring_color(knob);
}

// Scales the zone widths to `width` (>= kMinZoneWidth): proportionally when every zone keeps at least
// kMinZoneWidth, else kMinZoneWidth each plus a share of the rest in proportion to what each had
// above it. The last zone takes the rounding, so the sum is exact. Zones the strip cannot hold are dropped.
static void fit_strip(std::vector<ZoneConfig>& zones, int width) {
  if (zones.empty() || width < kMinZoneWidth) return;
  const std::size_t fits = static_cast<std::size_t>(width / kMinZoneWidth);
  if (zones.size() > fits) zones.resize(fits);
  long long total = 0;
  for (const ZoneConfig& z : zones) total += z.width;
  if (total == width) return;
  const std::size_t n = zones.size();
  std::vector<long long> w(n);
  long long used = 0;
  bool fine = total > 0;
  for (std::size_t i = 0; fine && i + 1 < n; ++i) {
    w[i] = zones[i].width * static_cast<long long>(width) / total;
    fine = w[i] >= kMinZoneWidth;
    used += w[i];
  }
  if (fine) fine = width - used >= kMinZoneWidth;
  if (!fine) {  // kMinZoneWidth each, then the rest shared by what each zone had above the minimum
    const long long rest = width - static_cast<long long>(n) * kMinZoneWidth;
    long long above = 0;
    for (const ZoneConfig& z : zones) above += std::max(z.width - kMinZoneWidth, 0);
    used = 0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
      const long long share = above > 0 ? rest * std::max(zones[i].width - kMinZoneWidth, 0) / above
                                        : rest / static_cast<long long>(n);
      w[i] = kMinZoneWidth + share;
      used += w[i];
    }
  }
  w[n - 1] = width - used;
  for (std::size_t i = 0; i < n; ++i) zones[i].width = static_cast<int>(w[i]);
}

void fit_to_shape(Config& c, const ConfigShape& shape) {
  const std::size_t keys = static_cast<std::size_t>(std::max(shape.keys, 0));
  const std::size_t knobs = static_cast<std::size_t>(std::max(shape.knobs, 0));
  if (c.keys.size() < keys) c.keys.resize(keys);
  if (c.knobs.size() < knobs) c.knobs.resize(knobs);
  while (c.ring_colors.size() < knobs) c.ring_colors.push_back(default_ring_color(static_cast<int>(c.ring_colors.size())));
  if (shape.has_strip) fit_strip(c.strip, shape.strip_width);
  c.model = shape.device;
}

std::vector<ConfigError> slots_beyond_device(const Config& next, const Config& current, const ConfigShape& shape) {
  std::vector<ConfigError> out;
  auto check = [&](const char* key, const std::vector<std::optional<SlotConfig>>& want,
                   const std::vector<std::optional<SlotConfig>>& have, int count, const char* one, const char* many) {
    for (std::size_t i = static_cast<std::size_t>(std::max(count, 0)); i < want.size(); ++i) {
      if (!want[i]) continue;  // emptied: harmless
      if (slot_json(want[i]) == slot_json(slot_at(have, static_cast<int>(i)))) continue;  // kept as it was
      out.push_back({std::format("{}[{}]", key, i), std::format("the {} has {} {}", shape.name, count, count == 1 ? one : many)});
    }
  };
  check("keys", next.keys, current.keys, shape.keys, "key", "keys");
  check("knobs", next.knobs, current.knobs, shape.knobs, "knob", "knobs");
  return out;
}

json errors_to_json(const std::vector<ConfigError>& errors) {
  json out = json::array();
  for (const auto& e : errors) out.push_back(json{{"path", e.path}, {"message", e.message}});
  return out;
}

}  // namespace ld
