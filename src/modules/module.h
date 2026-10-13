#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/http.h"
#include "core/types.h"
#include "modules/poster.h"
#include "render/renderer.h"

namespace ld {

enum class SlotKind { Key, Knob, StripZone };
enum class EventKind { Press, Release, Tap, LongPress, Rotate, SwipeLeft, SwipeRight };

struct InputEvent {
  EventKind kind;
  int delta = 0;  // Rotate: signed notches, < 0 counter-clockwise, > 0 clockwise (may be more than 1)
  int x = 0;      // strip zones: x relative to the zone's left edge
};

// Services the core gives a module. Timers and posts are cancelled when the module is destroyed.
// Everything runs on the loop thread. From other threads, use a Poster obtained with poster()
// (never call the context itself from another thread).
class ModuleContext {
public:
  virtual ~ModuleContext() = default;
  virtual TimePoint now() const = 0;
  virtual TimerId at(TimePoint when, std::function<void()> fn) = 0;
  virtual TimerId every(Duration period, std::function<void()> fn) = 0;
  virtual void cancel(TimerId id) = 0;
  virtual void post(std::function<void()> fn) = 0;
  virtual Poster poster() = 0;  // copyable, thread-safe; see modules/poster.h
  virtual void invalidate() = 0;                  // "my rendering changed"
  virtual void set_ring_color(Rgb color) = 0;     // knob slots only; ignored elsewhere
  virtual void clear_ring_color() = 0;
  // Asynchronous GET (see core/http.h): `done` runs on the loop thread, and is dropped if the module
  // is gone. A request may add one header, allow plain http, and pick the isolated lane.
  virtual void http_get(const HttpRequest& request, std::function<void(const HttpResponse&)> done) = 0;
  // An https url and nothing else, on the shared lane.
  void http_get(const std::string& url, std::function<void(const HttpResponse&)> done) {
    http_get(HttpRequest{.url = url}, std::move(done));
  }
  virtual nlohmann::json load_state() = 0;                 // this slot's persisted state (null if none)
  virtual void save_state(const nlohmann::json& state) = 0;
};

// One instance per placed slot. Never creates threads, never blocks.
class Module {
public:
  virtual ~Module() = default;
  virtual void start(ModuleContext& ctx, const nlohmann::json& settings) = 0;  // settings already validated
  virtual void on_event(const InputEvent&) {}
  virtual void render(Canvas& canvas) = 0;   // only called after invalidate(); canvas is the slot size
  virtual bool animating() const { return false; }  // true -> re-rendered at 30 fps while true
  virtual bool slow_animation() const { return false; }  // while animating: 20 fps instead of 30 (30 if another slot needs it)
};

// A state of a knob's ring. Its colour and brightness (0-100 %) are two settings of the
// module, named here so the page shows them as one line of the ring-state table.
struct RingStateSpec {
  const char* key;             // "normal", "muted", "playing", "stopped"
  const char* label;           // shown on the page, in English
  const char* color_key;       // the settings key of its colour
  const char* brightness_key;  // the settings key of its brightness
};

struct ModuleInfo {
  std::string id;
  std::string name;         // shown on the page: short; at most a short aside in parentheses ("Memory (RAM)")
  std::string description;  // one line under the name in the page's module catalogue (at most 70 characters)
  std::string category;     // its group in the catalogue: System, Time, Weather & sky, Media & sound, Actions
                            // or Online data
  std::string hue;          // "#rrggbb": the page's accent while the module is selected; "" = neutral
  std::vector<SlotKind> slots;
  nlohmann::json settings_schema = nlohmann::json{{"fields", nlohmann::json::array()}};
  std::vector<ElementSpec> elements;  // text and icon elements the page can resize
  std::vector<RingStateSpec> ring_states;  // knobs: the ring colour of each state
  std::function<std::unique_ptr<Module>()> create;
  // Optional: what the schema cannot check (a key combination, a length), on settings that passed
  // the schema. An error message such as `keys: unknown key "Ctrlx"` refuses the configuration (the
  // page shows it at keys[i].settings); nullopt accepts it.
  std::function<std::optional<std::string>(const nlohmann::json& settings)> check;
  bool allows(SlotKind kind) const;
};

using ModuleLookup = std::function<const ModuleInfo*(std::string_view)>;

// What is wrong with a module's ElementSpecs, one message per problem (empty when sound): a key
// that is null, empty or used twice; a missing label; `icon` that disagrees with `kind`; a role on an
// icon or a graph; and, on texts and icons, min_size > max_size, a non-finite size or a weight other
// than 400, 600 or 700. The renderer relies on these (std::clamp, key lookups, CreateTextFormat).
std::vector<std::string> element_spec_errors(std::span<const ElementSpec> elements);
// `modules` without the ones whose elements are unsound (logged with their id): such a module is
// unknown to the config, rather than undefined behaviour when it draws. module_registry() uses it.
std::vector<ModuleInfo> sound_modules(std::vector<ModuleInfo> modules);
const std::vector<ModuleInfo>& module_registry();
const ModuleInfo* find_module(std::string_view id);

}  // namespace ld
