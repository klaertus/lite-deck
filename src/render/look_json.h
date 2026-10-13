// src/render/look_json.h: the names of the appearance values in config.json and the web API.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "render/look.h"

namespace ld {

const char* row_name(Row row);  // "top", "middle", "bottom", "line1", "line2"
std::optional<Row> row_from_name(std::string_view name);
const char* align_name(HAlign align);  // "left", "center", "right"
std::optional<HAlign> align_from_name(std::string_view name);
const char* kind_name(ElementKind kind);       // "text", "graph", "icon"
const char* color_mode_name(ColorMode mode);   // "fixed", "state", "gradient"
// "#rrggbb" (a fixed colour), "state" or "gradient".
std::string color_choice_name(const ColorChoice& choice);
std::optional<ColorChoice> color_choice_from_name(std::string_view name);
// "label", "value", "long_value", "two_line", "secondary", "symbol"; "" for None.
const char* role_name(TextRole role);
std::optional<TextRole> role_from_name(std::string_view name);  // never None

}  // namespace ld
