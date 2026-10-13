// The fixtures of the page's mock server (tests/web/fixtures): the module catalogue and the device as
// GET /api/modules and GET /api/device answer them, and the default N4 Pro configuration. They must
// match the daemon: this test fails when they drift, and LD_UPDATE_WEB_FIXTURES=1 rewrites them
// (tests/web/fixtures only).
#include <doctest/doctest.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include "core/config.h"
#include "device/registry.h"
#include "web/api.h"
#include "web_test_util.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool updating() {
  char* v = nullptr;
  std::size_t n = 0;
  const bool on = _dupenv_s(&v, &n, "LD_UPDATE_WEB_FIXTURES") == 0 && v && std::strcmp(v, "1") == 0;
  free(v);
  return on;
}

void check_fixture(const char* name, const json& expected) {
  const fs::path file = fs::path(LD_TEST_DIR) / "web" / "fixtures" / name;
  if (updating()) {
    fs::create_directories(file.parent_path());
    testutil::write_text(file, expected.dump(1) + "\n");
    return;
  }
  INFO(file.string() << " is out of date: run the tests once with LD_UPDATE_WEB_FIXTURES=1");
  const std::string text = testutil::read_text(file);
  REQUIRE_FALSE(text.empty());
  CHECK(json::parse(text) == expected);
}

}  // namespace

TEST_CASE("the mock server's fixtures match the daemon's modules, device and default configuration") {
  const ld::DeviceLayout& layout = ld::default_device().layout;
  check_fixture("modules.json", ld::modules_to_json(ld::module_registry()));
  check_fixture("device.json", ld::layout_to_json(layout));
  check_fixture("config.json", ld::to_json(ld::default_config(ld::config_shape(layout))));
}
