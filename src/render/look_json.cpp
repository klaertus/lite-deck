// src/render/look_json.cpp
#include "render/look_json.h"
#include <array>
#include <utility>

namespace ld {
namespace {
constexpr std::array<std::pair<Row, const char*>, 5> kRows{{{Row::Top, "top"},
                                                            {Row::Middle, "middle"},
                                                            {Row::Bottom, "bottom"},
                                                            {Row::Line1, "line1"},
                                                            {Row::Line2, "line2"}}};
constexpr std::array<std::pair<HAlign, const char*>, 3> kAligns{
    {{HAlign::Left, "left"}, {HAlign::Center, "center"}, {HAlign::Right, "right"}}};
constexpr std::array<std::pair<TextRole, const char*>, 6> kRoles{{{TextRole::Label, "label"},
                                                                  {TextRole::Value, "value"},
                                                                  {TextRole::LongValue, "long_value"},
                                                                  {TextRole::TwoLine, "two_line"},
                                                                  {TextRole::Secondary, "secondary"},
                                                                  {TextRole::Symbol, "symbol"}}};
}  // namespace

const char* row_name(Row row) {
  for (const auto& [r, n] : kRows)
    if (r == row) return n;
  return "middle";
}

std::optional<Row> row_from_name(std::string_view name) {
  for (const auto& [r, n] : kRows)
    if (name == n) return r;
  return std::nullopt;
}

const char* align_name(HAlign align) {
  for (const auto& [a, n] : kAligns)
    if (a == align) return n;
  return "center";
}

std::optional<HAlign> align_from_name(std::string_view name) {
  for (const auto& [a, n] : kAligns)
    if (name == n) return a;
  return std::nullopt;
}

const char* kind_name(ElementKind kind) {
  return kind == ElementKind::Graph ? "graph" : kind == ElementKind::Icon ? "icon" : "text";
}

const char* color_mode_name(ColorMode mode) {
  return mode == ColorMode::State ? "state" : mode == ColorMode::Gradient ? "gradient" : "fixed";
}

std::string color_choice_name(const ColorChoice& c) {
  if (c.mode == ColorMode::State) return "state";
  if (c.mode == ColorMode::Gradient) return "gradient";
  return format_color(c.color);
}

std::optional<ColorChoice> color_choice_from_name(std::string_view name) {
  if (name == "state") return ColorChoice{ColorMode::State, {}};
  if (name == "gradient") return ColorChoice{ColorMode::Gradient, {}};
  if (auto c = parse_color(name)) return ColorChoice{ColorMode::Fixed, *c};
  return std::nullopt;
}

const char* role_name(TextRole role) {
  for (const auto& [r, n] : kRoles)
    if (r == role) return n;
  return "";
}

std::optional<TextRole> role_from_name(std::string_view name) {
  for (const auto& [r, n] : kRoles)
    if (name == n) return r;
  return std::nullopt;
}

}  // namespace ld
