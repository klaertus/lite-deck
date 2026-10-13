#include <doctest/doctest.h>
#include "core/types.h"

TEST_CASE("parse_color accepts #rrggbb only") {
  CHECK(ld::parse_color("#3e0000") == ld::Rgb{0x3e, 0, 0});
  CHECK(ld::parse_color("#FFaa01") == ld::Rgb{0xff, 0xaa, 0x01});
  CHECK_FALSE(ld::parse_color("3e0000"));
  CHECK_FALSE(ld::parse_color("#3e00"));
  CHECK_FALSE(ld::parse_color("#zz0000"));
  CHECK(ld::format_color({0x1b, 0x35, 0x00}) == "#1b3500");
}
