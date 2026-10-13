// src/core/config_migration.h: migration of a v1 config (before the design system) to the current format.
#pragma once
#include <nlohmann/json.hpp>

namespace ld {

struct Migration {
  nlohmann::json doc;    // migrated, or the input unchanged
  bool changed = false;  // true: `doc` differs and should be written back once
};

// Pure and idempotent. On keys only: a module's colour setting moves to elements.<primary>.color
// unless it equals that module's old default (then the new default applies), and is removed in
// every case; the old element keys "prices" (crypto_gold) and "times" (sun) become line1 and
// line2 (and the symbols, and sun's status, which they also sized). Everything else is kept as
// written, including what the parser will refuse. appearance.states is never written (absent =
// the palette defaults in code). A document whose appearance is not an object is returned
// unchanged. A migrated document has nothing left to migrate: `changed` is false the second time.
Migration migrate_v1(const nlohmann::json& doc);

}  // namespace ld
