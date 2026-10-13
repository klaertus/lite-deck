// src/core/config.h
#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/types.h"
#include "modules/module.h"
#include "render/look.h"

namespace ld {

struct SlotConfig {
  std::string module;
  nlohmann::json settings = nlohmann::json::object();
  // Appearance, next to the settings: changing it redraws the slot without restarting
  // its module. "" / nullopt = the global appearance; `elements` holds only what the user changed.
  std::string font;
  std::optional<bool> italic;
  ElementOverrides elements;
};

// What the user set for a text role of the keys (appearance.roles.<role>);
// nullopt / "" = not set. The font may be missing on this PC (the Deck falls back).
struct RoleOverride {
  std::optional<int> size;    // kMinRoleSize-kMaxRoleSize px at the standard key height
  std::optional<int> weight;  // 400, 600 or 700
  std::string font;
  bool operator==(const RoleOverride&) const = default;
};
using RoleOverrides = std::map<TextRole, RoleOverride>;

struct Appearance {
  std::string font = "Segoe UI";  // never empty; may name a font missing on this PC (Deck falls back)
  bool italic = false;
  StatePalette states;  // warning, critical, idle, stale (config appearance.states)
  RoleOverrides roles;  // only the roles the user set (keys only)
  bool operator==(const Appearance&) const = default;
};

inline constexpr const char* kDefaultDevice = "mirabox-n4pro";  // device.model when the file names none
constexpr int kDefaultStripWidth = 800;
constexpr int kMaxConfigStripWidth = 4096;  // = kMaxStripWidth: a strip section written on any device

struct ZoneConfig {
  int width = kDefaultStripWidth;
  std::optional<SlotConfig> slot;  // nullopt = empty zone
};

constexpr std::size_t kMaxConfigKeys = 64;   // = kMaxDeviceKeys (device/device.h)
constexpr std::size_t kMaxConfigKnobs = 16;  // = kMaxDeviceKnobs
constexpr std::size_t kMaxModelLength = 64;  // = kMaxDeviceIdLength

// The default ring colours: the four of the N4 Pro, then dark grey (#202020).
Rgb default_ring_color(int knob);
std::vector<Rgb> default_ring_colors();  // the four N4 Pro ones

struct Config {
  int version = 1;
  std::string model = kDefaultDevice;  // device.model: the device this config goes with (written if not the N4 Pro)
  int brightness = 60;
  int ring_brightness = 100;
  std::vector<Rgb> ring_colors = default_ring_colors();  // per knob; missing ones: default_ring_color
  int web_port = 7878;
  Appearance appearance;
  // Left to right, then top to bottom. Slots the device does not have are kept (never shown or started).
  std::vector<std::optional<SlotConfig>> keys = std::vector<std::optional<SlotConfig>>(10);
  std::vector<std::optional<SlotConfig>> knobs = std::vector<std::optional<SlotConfig>>(4);
  std::vector<ZoneConfig> strip{ZoneConfig{}};
};

struct ConfigError {
  std::string path;
  std::string message;
};

struct ParseResult {
  std::optional<Config> config;
  std::vector<ConfigError> errors;
};

struct LoadResult {
  std::optional<Config> config;
  std::vector<ConfigError> errors;
  bool missing = false;
  nlohmann::json doc;  // the document as read (null if missing or unreadable)
  bool migrated = false;  // `doc` is the migration of a v1 file (config_migration.h): write it back once
};

// What the active device offers, as the config sees it (device/registry.h: config_shape). The default
// is the N4 Pro, so a config read without a device is read as before.
struct ConfigShape {
  std::string device = kDefaultDevice;
  std::string name = "MiraBox N4 Pro E";
  int keys = 10;
  int knobs = 4;
  int strip_width = kDefaultStripWidth;  // without a strip: kDefaultStripWidth (the section is kept as is)
  bool has_strip = true;                 // false: the strip section is kept as is, never fitted
  bool operator==(const ConfigShape&) const = default;
};

// Slot `i` of `slots`, or an empty slot when the list is shorter.
const std::optional<SlotConfig>& slot_at(const std::vector<std::optional<SlotConfig>>& slots, int i);
// The ring colour of `knob`: the configured one, or default_ring_color.
Rgb ring_color(const Config& c, int knob);
// Brings `c` to the device: keys, knobs and ring colours filled up to its counts (empty slots,
// default colours), never cut; the model becomes the device's. On a device with a strip, zones whose
// widths do not add up to its width are scaled to it (at least kMinZoneWidth each, the last zone
// takes the rounding; trailing zones are dropped only when the strip cannot hold them all).
void fit_to_shape(Config& c, const ConfigShape& shape);
// The slots of `next` beyond the device (index >= shape.keys / shape.knobs) that hold a module other
// than the one `current` keeps there: the page cannot place a module where the device has no key.
std::vector<ConfigError> slots_beyond_device(const Config& next, const Config& current, const ConfigShape& shape);

constexpr int kMinZoneWidth = 60;
constexpr std::size_t kMaxFontName = 100;
constexpr int kMinRoleSize = 8;
constexpr int kMaxRoleSize = 60;

// The strip widths must add up to shape.strip_width when the document goes with this device (its
// device.model is shape.device) and the device has a strip. A document from another device (or any
// document on a device without a strip) only needs valid zones of at most kMaxConfigStripWidth in all:
// fit_to_shape then brings them to the strip.
ParseResult parse_config(const nlohmann::json& j, const ModuleLookup& lookup, const ConfigShape& shape = {});
nlohmann::json to_json(const Config& c);
// {"warning": "#ffb000", "critical": "#ff453a", "idle": "#…", "stale": "#…"}.
nlohmann::json states_to_json(const StatePalette& states);
Config default_config();
// default_config() brought to the device (fit_to_shape): strip zones sized to its strip.
Config default_config(const ConfigShape& shape);
LoadResult load_config_file(const std::filesystem::path& file, const ModuleLookup& lookup, const ConfigShape& shape = {});
// device.model of `file`, read without validating the rest (the active device is chosen before the
// config is parsed); "" when absent, unreadable or not an id of 1 to kMaxModelLength characters.
std::string configured_device(const std::filesystem::path& file);
// Writes <file>.tmp, flushes it to disk, then replaces <file> atomically. `extra` = keys the parser
// does not know (see unknown_keys), written back unchanged. Removes the temp file on failure.
bool save_config_file(const std::filesystem::path& file, const Config& c,
                      const nlohmann::json& extra = nlohmann::json::object());
// Copies `file` to `backup` unless `backup` already exists (an older backup is never overwritten).
// True if `backup` exists afterwards.
bool backup_once(const std::filesystem::path& file, const std::filesystem::path& backup);

// Keys of a config document that parse_config ignores (top level, "device", "web"), same shape.
nlohmann::json unknown_keys(const nlohmann::json& doc);
// `canonical` plus the keys of `extra` it does not already have.
nlohmann::json with_unknown_keys(nlohmann::json canonical, const nlohmann::json& extra);
// [{"path": ..., "message": ...}], for the web API and the event stream.
nlohmann::json errors_to_json(const std::vector<ConfigError>& errors);

}  // namespace ld
