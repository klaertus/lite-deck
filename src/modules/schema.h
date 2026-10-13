#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace ld {

// schema = {"fields":[{"key","type","default",["min"],["max"],["options"],"label",["hint"],["placeholder"],
//                     ["group"],["show_if"]}]}
// type: int | number | bool | string | secret (a string the page masks) | color (#rrggbb) | enum.
// enum options: values ("open"), or {"value": "open", "label": "Open a link, file or program"} for the page.
// show_if: {"field": <another key>, "equals": <value>} or {"field": ..., "in": [values]}, optionally with
// "and": <another such condition>, or a list of conditions (any one is enough): the page shows the
// field only then (the daemon still checks and keeps it). Not chained: only the named field's value
// counts, not whether that field is shown. label: a few words; help goes to hint.
// Returns an error message, or nullopt with `out` = validated settings + defaults (unknown keys dropped).
std::optional<std::string> apply_schema(const nlohmann::json& schema, const nlohmann::json& in, nlohmann::json& out);

}  // namespace ld
