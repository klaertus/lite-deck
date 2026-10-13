#include <doctest/doctest.h>
#include <climits>
#include "modules/common/format.h"

TEST_CASE("format_decimal uses a decimal point and rounds") {
  CHECK(ld::format_decimal(12.468, 2) == L"12.47");
  CHECK(ld::format_decimal(3.0, 2) == L"3.00");
  CHECK(ld::format_decimal(54.4, 0) == L"54");
}

TEST_CASE("format_grouped separates thousands with a narrow no-break space") {
  CHECK(ld::format_grouped(98512) == L"98 512");
  CHECK(ld::format_grouped(1234567) == L"1 234 567");
  CHECK(ld::format_grouped(999) == L"999");
  CHECK(ld::format_grouped(-2431) == L"-2 431");
}

TEST_CASE("format_grouped handles LLONG_MIN") {
  CHECK(ld::format_grouped(LLONG_MIN) == L"-9 223 372 036 854 775 808");
}
