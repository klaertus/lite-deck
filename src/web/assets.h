#pragma once
#include <optional>
#include <string_view>
#include "web/api.h"

namespace ld {

// The config page, embedded in the executable as RCDATA resources (src/web/assets/web.rc): "/" and
// one path per file web.rc lists. Points into the mapped image: no copy, no disk access.
std::optional<Asset> embedded_asset(std::string_view path);

}  // namespace ld
