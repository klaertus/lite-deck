// src/render/look.h
#pragma once
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include "core/types.h"

namespace ld {

inline constexpr const wchar_t* kDefaultFont = L"Segoe UI";

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Center, Bottom };

// What an element is: text, a graph drawn as a line, or an icon (colour emoji in Segoe UI).
enum class ElementKind { Text, Graph, Icon };
// The rows of the key template: Top / Middle / Bottom, or Line1 / Line2 on two-line keys.
enum class Row { Top, Middle, Bottom, Line1, Line2 };
// How an element is coloured: one colour, the state palette, or a gradient on a metric.
enum class ColorMode { Fixed, State, Gradient };
// What a value means right now.
enum class ElementState { Normal, Warning, Critical, Idle, Stale };
// The role of a key text: the Device panel sets the font, size and weight of
// all the elements of a role at once, on every key. None: icons, graphs and the strip's elements.
enum class TextRole { None, Label, Value, LongValue, TwoLine, Secondary, Symbol };
// The six roles, in the order of the Device panel.
inline constexpr std::array<TextRole, 6> kTextRoles{TextRole::Label,   TextRole::Value,     TextRole::LongValue,
                                                    TextRole::TwoLine, TextRole::Secondary, TextRole::Symbol};

// An element a module shows, declared in its
// ModuleInfo. `size` is the default in px at the standard slot height (112 px). Icons keep Segoe UI
// and stay upright whatever the slot font (colour emoji), and have no weight; graphs have neither
// size nor weight. Initialise with designated initialisers, in the order of the members.
struct ElementSpec {
  const char* key;
  const char* label;  // shown on the page, in English
  float size;
  int weight;  // 400, 600 or 700
  int min_size;
  int max_size;
  bool icon = false;  // true exactly when kind == ElementKind::Icon (older code reads it)
  ElementKind kind = ElementKind::Text;
  Row row = Row::Middle;
  HAlign align = HAlign::Center;
  ColorMode color_mode = ColorMode::Fixed;
  Rgb color{255, 255, 255};  // the "normal" colour
  bool visible = true;
  bool keeps_color = false;  // labels and units: idle / stale apply only if the user picks "state"
  bool gradient = false;     // the module gives a metric: "gradient" may be chosen for this element
  TextRole role = TextRole::None;  // every text of a key module has one; a test checks it
};

// The user's colour choice for an element (config "color": "#rrggbb", "state" or "gradient").
struct ColorChoice {
  ColorMode mode = ColorMode::Fixed;
  Rgb color{};  // Fixed only
  bool operator==(const ColorChoice&) const = default;
};

// What the user changed on one element; nullopt = the module's default.
struct ElementOverride {
  std::optional<int> size;
  std::optional<int> weight;
  std::optional<bool> visible;
  std::optional<Row> row;
  std::optional<HAlign> align;
  std::optional<ColorChoice> color;
  bool operator==(const ElementOverride&) const = default;
};
using ElementOverrides = std::map<std::string, ElementOverride, std::less<>>;

// The global state colours (config appearance.states). "Normal" is each element's colour.
struct StatePalette {
  Rgb warning{0xff, 0xb0, 0x00};
  Rgb critical{0xff, 0x45, 0x3a};
  Rgb idle{0x8c, 0x8c, 0x8c};
  Rgb stale{0x5f, 0x5f, 0x5f};
  bool operator==(const StatePalette&) const = default;
};

// A text role as one key draws it, resolved by the Deck from appearance.roles:
// the size (px at the standard 112 px height) and weight the user set, and the role's font when it
// applies: an installed family; "" = the slot's font (none set, not installed, or the key names its own).
struct RoleLook {
  std::optional<int> size;
  std::optional<int> weight;
  std::wstring font;
  bool operator==(const RoleLook&) const = default;
};
using RoleLooks = std::map<TextRole, RoleLook>;

// A three-point colour scale: cold, mid and hot, each a value and a colour.
struct GradientStop {
  double at = 0;
  Rgb color{};
  bool operator==(const GradientStop&) const = default;
};
struct Gradient {
  std::array<GradientStop, 3> stops{};
  bool operator==(const Gradient&) const = default;
};

// What the module says about an element when it is drawn: its state, a metric for a gradient, and
// defaults it computes (a size from the slot height, a colour per mode). The user's settings win.
struct ElementInput {
  ElementState state = ElementState::Normal;
  std::optional<double> metric;
  std::optional<Gradient> gradient;
  std::optional<Rgb> color;
  std::optional<float> size;
};

// One element once the module's defaults, the user's settings and the state are applied.
struct ResolvedStyle {
  bool visible = true;
  Row row = Row::Middle;
  HAlign align = HAlign::Center;
  float size = 20;
  int weight = 400;
  Rgb color{255, 255, 255};
  std::wstring font;  // the role's font; "" = the slot's font
};

// The effective appearance of one slot, resolved by the Deck: an installed family (never empty),
// italic or not, the element overrides, the state palette and, on a key, the text roles.
struct SlotLook {
  std::wstring font = kDefaultFont;
  bool italic = false;
  ElementOverrides elements;
  StatePalette states;
  RoleLooks roles;  // keys only; empty (no role set): the modules' defaults, the same pixels
  bool operator==(const SlotLook&) const = default;
};

}  // namespace ld
