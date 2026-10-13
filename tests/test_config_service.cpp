#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include "core/config_service.h"
#include "web_test_util.h"

using nlohmann::json;
using namespace testutil;
namespace fs = std::filesystem;

namespace {
struct Dir {
  fs::path path;
  explicit Dir(const char* name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path file() const { return path / "config.json"; }
};

std::string city_of(const ld::ConfigService& s) { return s.current().keys[0]->settings["city"].get<std::string>(); }
}  // namespace

TEST_CASE("ConfigService loads the file, keeps its unknown keys and removes a stale temp file") {
  Dir d("ld_svc_load");
  json doc = doc_with_city("A");
  doc["comment"] = "keep";
  doc["device"] = {{"brightness", 40}, {"future", true}};
  write_text(d.file(), doc.dump());
  write_text(d.path / "config.json.tmp", "left by a crash");
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  CHECK(city_of(s) == "A");
  CHECK(s.current().brightness == 40);
  CHECK(s.revision() == 1);
  CHECK(s.last_errors().empty());
  CHECK_FALSE(fs::exists(d.path / "config.json.tmp"));
  CHECK(s.document()["comment"] == "keep");
  CHECK(s.document()["device"]["future"] == true);
}

TEST_CASE("ConfigService::load writes the default when the file is missing and reports an invalid one") {
  Dir d("ld_svc_default");
  {
    ld::ConfigService s(d.file(), ld::find_module);
    s.load();
    CHECK(fs::exists(d.file()));
    CHECK(s.last_errors().empty());
  }
  write_text(d.file(), "{ broken");
  ld::ConfigService s(d.file(), ld::find_module);
  s.load();
  CHECK_FALSE(s.last_errors().empty());
  CHECK(read_text(d.file()) == "{ broken");  // left untouched
}

TEST_CASE("update validates, writes with unknown keys, then applies") {
  Dir d("ld_svc_update");
  json doc = doc_with_city("A");
  doc["comment"] = "keep";
  write_text(d.file(), doc.dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  int applied = 0, statuses = 0;
  std::string before, after;
  s.on_applied = [&](const ld::Config& b, const ld::Config& a) {
    ++applied;
    before = b.keys[0]->settings["city"].get<std::string>();
    after = a.keys[0]->settings["city"].get<std::string>();
  };
  s.on_status = [&] { ++statuses; };
  json next = s.document();
  next["keys"][0]["settings"]["city"] = "B";
  ld::UpdateResult r = s.update(next, s.revision());
  CHECK(r.status == ld::UpdateStatus::Applied);
  CHECK(r.revision == 2);
  CHECK(s.revision() == 2);
  CHECK(applied == 1);
  CHECK(statuses == 1);
  CHECK(before == "A");
  CHECK(after == "B");
  json on_disk = json::parse(read_text(d.file()));
  CHECK(on_disk["keys"][0]["settings"]["city"] == "B");
  CHECK(on_disk["comment"] == "keep");
}

TEST_CASE("update rejects an invalid document and changes nothing") {
  Dir d("ld_svc_invalid");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  int applied = 0;
  s.on_applied = [&](const ld::Config&, const ld::Config&) { ++applied; };
  const std::string text = read_text(d.file());
  json bad = s.document();
  bad["strip"][0]["width"] = 700;           // sum != 800
  bad["keys"][1] = {{"module", "nope"}};    // unknown module
  ld::UpdateResult r = s.update(bad, 0);
  CHECK(r.status == ld::UpdateStatus::Invalid);
  CHECK(r.errors.size() == 2);
  CHECK(r.revision == 1);
  CHECK(applied == 0);
  CHECK(read_text(d.file()) == text);
  CHECK(city_of(s) == "A");
}

TEST_CASE("update with a stale revision is a conflict") {
  Dir d("ld_svc_conflict");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  json next = s.document();
  next["keys"][0]["settings"]["city"] = "B";
  CHECK(s.update(next, 99).status == ld::UpdateStatus::Conflict);
  CHECK(city_of(s) == "A");
  CHECK(s.update(next, 0).status == ld::UpdateStatus::Applied);  // 0 = no check (curl, scripts)
}

TEST_CASE("update with the same content is Unchanged and does not rewrite the file") {
  Dir d("ld_svc_same");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  int applied = 0;
  s.on_applied = [&](const ld::Config&, const ld::Config&) { ++applied; };
  const auto stamp = fs::last_write_time(d.file());
  ld::UpdateResult r = s.update(s.document(), s.revision());
  CHECK(r.status == ld::UpdateStatus::Unchanged);
  CHECK(applied == 0);
  CHECK(fs::last_write_time(d.file()) == stamp);
}

TEST_CASE("reload applies an external edit and reports an invalid one without dropping the running config") {
  Dir d("ld_svc_reload");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  int applied = 0, statuses = 0;
  s.on_applied = [&](const ld::Config&, const ld::Config&) { ++applied; };
  s.on_status = [&] { ++statuses; };
  s.reload();  // our own content: nothing to do
  CHECK(applied == 0);
  CHECK(statuses == 0);
  write_text(d.file(), doc_with_city("B").dump());
  s.reload();
  CHECK(city_of(s) == "B");
  CHECK(s.revision() == 2);
  CHECK(applied == 1);
  write_text(d.file(), "{ \"version\": 1, oops");
  s.reload();
  CHECK(city_of(s) == "B");
  CHECK_FALSE(s.last_errors().empty());
  CHECK(statuses == 2);
  write_text(d.file(), doc_with_city("B").dump());  // fixed back to the running content
  s.reload();
  CHECK(s.last_errors().empty());
  CHECK(statuses == 3);
  CHECK(applied == 1);
}

TEST_CASE("reload of a deleted file writes the running config back") {
  Dir d("ld_svc_deleted");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  fs::remove(d.file());
  s.reload();
  REQUIRE(fs::exists(d.file()));
  CHECK(json::parse(read_text(d.file()))["keys"][0]["settings"]["city"] == "A");
  CHECK(s.last_errors().empty());
}

TEST_CASE("non-ASCII settings round-trip byte for byte") {
  Dir d("ld_svc_utf8");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  const std::string city = "Liège - 東京 ✓";
  json next = s.document();
  next["keys"][0]["settings"]["city"] = city;
  REQUIRE(s.update(next, 0).status == ld::UpdateStatus::Applied);
  CHECK(read_text(d.file()).find(city) != std::string::npos);  // raw UTF-8, not \u escapes
  ld::ConfigService again(d.file(), city_lookup);
  again.load();
  CHECK(city_of(again) == city);
}

TEST_CASE("update reports a write failure and keeps the running config") {
  Dir d("ld_svc_writefail");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  fs::remove(d.file());
  fs::create_directories(d.file());  // a folder where config.json should be: the rename fails
  json next = s.document();
  next["keys"][0]["settings"]["city"] = "B";
  ld::UpdateResult r = s.update(next, 0);
  CHECK(r.status == ld::UpdateStatus::WriteFailed);
  CHECK_FALSE(r.errors.empty());
  CHECK(city_of(s) == "A");
  CHECK_FALSE(fs::exists(d.path / "config.json.tmp"));
}

TEST_CASE("an old config restored by hand while running is migrated by the reload, once") {
  Dir d("ld_svc_migrate");
  ld::ConfigService svc(d.file(), ld::find_module);
  svc.load();  // missing: writes the default (already in the new format)
  CHECK_FALSE(fs::exists(d.path / "config.json.v1.bak"));
  json old = ld::to_json(ld::default_config());
  int clock = -1;  // wherever the default layout puts it
  for (int i = 0; i < 10; ++i)
    if (old["keys"][i].is_object() && old["keys"][i]["module"] == "clock") clock = i;
  REQUIRE(clock >= 0);
  old["keys"][clock]["settings"]["color"] = "#ffcc00";  // a v1 module colour, chosen
  { std::ofstream(d.file(), std::ios::binary) << old.dump(2); }
  svc.reload();
  CHECK(fs::exists(d.path / "config.json.v1.bak"));
  CHECK(svc.current().keys[clock]->elements.at("time").color ==
        ld::ColorChoice{ld::ColorMode::Fixed, ld::Rgb{0xff, 0xcc, 0x00}});
  std::ifstream f(d.file(), std::ios::binary);
  const json now = json::parse(f);
  CHECK_FALSE(now["keys"][clock]["settings"].contains("color"));
  CHECK(now["keys"][clock]["elements"]["time"]["color"] == "#ffcc00");
  CHECK_FALSE(now["appearance"].contains("states"));  // the palette stays at its defaults, in code
}

TEST_CASE("an N4 Pro config.json is kept byte for byte: read, compared and written back without a device model") {
  Dir d("ld_svc_n4_bytes");
  json doc = doc_with_city("A");
  doc["device"] = {{"brightness", 40}, {"ring_brightness", 90},
                   {"ring_colors", {"#3e0000", "#002132", "#1b3500", "#3d1f00"}}, {"future", 1}};
  doc["comment"] = "keep";
  {
    ld::ConfigService first(d.file(), city_lookup);
    first.load();
    REQUIRE(first.update(doc, 0).status == ld::UpdateStatus::Applied);  // writes the canonical file
  }
  const std::string written = read_text(d.file());
  ld::ConfigService s(d.file(), city_lookup);  // the default shape: the N4 Pro
  s.load();
  CHECK(read_text(d.file()) == written);  // not rewritten at load
  CHECK_FALSE(s.document()["device"].contains("model"));
  CHECK(s.document() == json::parse(written));
  CHECK(s.update(json::parse(written), 0).status == ld::UpdateStatus::Unchanged);
  CHECK(read_text(d.file()) == written);
}

TEST_CASE("ConfigService on another device: fills in memory, records the model once, and gives it back") {
  Dir d("ld_svc_shape");
  write_text(d.file(), doc_with_city("A").dump());
  const ld::ConfigShape deck15{"test-deck", "Test Deck", 15, 2, 800};
  {
    ld::ConfigService s(d.file(), city_lookup, deck15);
    s.load();
    CHECK(s.current().keys.size() == 15);
    CHECK(s.current().model == "test-deck");
    CHECK(city_of(s) == "A");
    CHECK(json::parse(read_text(d.file()))["device"]["model"] == "test-deck");  // recorded
  }
  ld::ConfigService n4(d.file(), city_lookup);  // back on an N4 Pro
  n4.load();
  CHECK(n4.current().model == ld::kDefaultDevice);
  CHECK(n4.current().keys.size() == 15);  // keys 10-14 (empty) stay in the file
  CHECK_FALSE(json::parse(read_text(d.file()))["device"].contains("model"));
}

TEST_CASE("update refuses a module outside the device and accepts the extra slots sent back unchanged") {
  Dir d("ld_svc_beyond");
  json doc = doc_with_city("A");
  for (int i = 10; i < 15; ++i) doc["keys"].push_back(nullptr);
  doc["keys"][12] = {{"module", "city"}, {"settings", {{"city", "Ghent"}}}};
  write_text(d.file(), doc.dump());
  ld::ConfigService s(d.file(), city_lookup);  // N4 Pro: 10 keys
  s.load();
  REQUIRE(s.last_errors().empty());
  CHECK(s.current().keys.size() == 15);  // kept, not shown
  json page = s.document();               // what the page sends back
  page["keys"][0]["settings"]["city"] = "B";
  CHECK(s.update(page, 0).status == ld::UpdateStatus::Applied);
  CHECK(json::parse(read_text(d.file()))["keys"][12]["settings"]["city"] == "Ghent");
  page = s.document();
  page["keys"][13] = {{"module", "city"}, {"settings", {{"city", "C"}}}};
  const auto r = s.update(page, 0);
  CHECK(r.status == ld::UpdateStatus::Invalid);
  REQUIRE(r.errors.size() == 1);
  CHECK(r.errors[0].path == "keys[13]");
  CHECK(r.errors[0].message == "the MiraBox N4 Pro E has 10 keys");
}

// The shape of a real N4 Pro config.json, as the daemon writes it (every module of the default layout,
// text roles, strip element overrides, tuned knob colours).
const char* const kRealN4ProConfig = R"({
  "appearance": {
    "font": "Segoe UI",
    "italic": false,
    "roles": {
      "label": {
        "size": 20
      },
      "long_value": {
        "size": 32
      },
      "secondary": {
        "size": 20
      },
      "symbol": {
        "size": 22
      },
      "two_line": {
        "size": 32
      },
      "value": {
        "size": 40
      }
    }
  },
  "device": {
    "brightness": 100,
    "ring_brightness": 55,
    "ring_colors": [
      "#3e0000",
      "#002132",
      "#1b3500",
      "#3d1f00"
    ]
  },
  "keys": [
    {
      "module": "cpu_load",
      "settings": {
        "cold_at": 40,
        "cold_color": "#4fa3ff",
        "crit": 90,
        "hot_at": 90,
        "hot_color": "#ff5a3c",
        "hysteresis": 3,
        "mid_at": 65,
        "mid_color": "#ffffff",
        "show_temperature": true,
        "tap_action": "task_manager",
        "tap_target": "",
        "warn": 80
      }
    },
    {
      "module": "gpu",
      "settings": {
        "cold_at": 40,
        "cold_color": "#4fa3ff",
        "hot_at": 90,
        "hot_color": "#ff5a3c",
        "hysteresis": 3,
        "index": 0,
        "mid_at": 65,
        "mid_color": "#ffffff",
        "mode": "usage",
        "show_temperature": true,
        "tap_action": "task_manager",
        "tap_target": "",
        "temp_crit": 85,
        "temp_warn": 75,
        "vram_crit": 97,
        "vram_warn": 90
      }
    },
    {
      "module": "ram",
      "settings": {
        "crit": 95,
        "hysteresis": 3,
        "tap_action": "task_manager",
        "tap_target": "",
        "warn": 85
      }
    },
    {
      "module": "gpu",
      "settings": {
        "cold_at": 40,
        "cold_color": "#4fa3ff",
        "hot_at": 90,
        "hot_color": "#ff5a3c",
        "hysteresis": 3,
        "index": 0,
        "mid_at": 65,
        "mid_color": "#ffffff",
        "mode": "vram",
        "show_temperature": false,
        "tap_action": "task_manager",
        "tap_target": "",
        "temp_crit": 85,
        "temp_warn": 75,
        "vram_crit": 97,
        "vram_warn": 90
      }
    },
    {
      "module": "stopwatch",
      "settings": {
        "file": "",
        "seconds_below": false
      }
    },
    {
      "module": "clock",
      "settings": {
        "seconds": true,
        "seconds_below": false,
        "tap_action": "open",
        "tap_target": "ms-clock:"
      }
    },
    {
      "module": "date",
      "settings": {
        "tap_action": "none",
        "tap_target": ""
      }
    },
    {
      "module": "weather",
      "settings": {
        "city": "Testville",
        "cold_at": -8,
        "cold_color": "#4fa3ff",
        "gradient_basis": "anomaly",
        "hot_at": 8,
        "hot_color": "#ff5a3c",
        "mid_at": 0,
        "mid_color": "#ffffff",
        "tap_action": "open",
        "tap_target": "https://open-meteo.com"
      }
    },
    {
      "module": "sun",
      "settings": {
        "city": "Testville",
        "verify_days": 7
      }
    },
    {
      "module": "crypto_gold",
      "settings": {}
    }
  ],
  "knobs": [
    {
      "module": "volume",
      "settings": {
        "brightness": 34,
        "color": "#ff0000",
        "muted_brightness": 0,
        "muted_color": "#3a3a3a",
        "step": 2,
        "windows_overlay": true
      }
    },
    {
      "module": "mic",
      "settings": {
        "brightness": 50,
        "color": "#1201fe",
        "muted_brightness": 0,
        "muted_color": "#3300ff",
        "step": 2
      }
    },
    {
      "module": "media",
      "settings": {
        "brightness": 56,
        "color": "#032900"
      }
    },
    {
      "module": "seek",
      "settings": {
        "brightness": 49,
        "color": "#4d2200",
        "idle_brightness": 0,
        "idle_color": "#3d1f00",
        "step_s": 10
      }
    }
  ],
  "strip": [
    {
      "elements": {
        "icon": {
          "size": 24
        },
        "idle": {
          "weight": 700
        },
        "title": {
          "size": 31
        }
      },
      "module": "now_playing",
      "settings": {
        "color": "#ffffff",
        "scroll_speed": 2
      },
      "width": 400
    },
    {
      "elements": {
        "age": {
          "size": 20
        },
        "app": {
          "size": 31
        },
        "count": {
          "size": 24
        },
        "empty": {
          "size": 26,
          "weight": 700
        },
        "status": {
          "size": 26
        }
      },
      "module": "notifications",
      "settings": {
        "bg_color": "#000000",
        "scroll_speed": 2,
        "text_color": "#ffffff"
      },
      "width": 400
    }
  ],
  "version": 1,
  "web": {
    "port": 7878
  }
}
)";

TEST_CASE("a real N4 Pro config.json round-trips byte for byte: parse and save, load, page update") {
  const std::string text = kRealN4ProConfig;
  const auto parsed = ld::parse_config(json::parse(text), ld::find_module);
  REQUIRE(parsed.errors.empty());
  REQUIRE(parsed.config);
  CHECK(parsed.config->model == ld::kDefaultDevice);
  CHECK(parsed.config->keys.size() == 10);
  CHECK(parsed.config->knobs.size() == 4);
  Dir d("ld_svc_real_n4");
  REQUIRE(ld::save_config_file(d.file(), *parsed.config));
  CHECK(read_text(d.file()) == text);  // to_json writes exactly what was read
  write_text(d.file(), text);
  ld::ConfigService s(d.file(), ld::find_module);
  s.load();
  CHECK(s.last_errors().empty());
  CHECK(read_text(d.file()) == text);  // not rewritten at load
  CHECK(s.document() == json::parse(text));
  CHECK(s.update(s.document(), 0).status == ld::UpdateStatus::Unchanged);
  json page = s.document();
  page["device"]["brightness"] = 99;
  REQUIRE(s.update(page, 0).status == ld::UpdateStatus::Applied);
  page["device"]["brightness"] = 100;
  REQUIRE(s.update(page, 0).status == ld::UpdateStatus::Applied);
  CHECK(read_text(d.file()) == text);  // written back as it was
}

// A config written before the tap actions: loading it leaves the file alone (the defaults are filled in
// memory); the next save writes them, like any setting a module gains.
TEST_CASE("a config from before the tap actions is not rewritten at load; the next save adds their defaults") {
  json old = json::parse(kRealN4ProConfig);
  for (json& k : old["keys"])
    if (k.is_object()) {
      k["settings"].erase("tap_action");
      k["settings"].erase("tap_target");
    }
  const std::string text = old.dump(2) + "\n";
  Dir d("ld_svc_pre_tap");
  write_text(d.file(), text);
  ld::ConfigService s(d.file(), ld::find_module);
  s.load();
  REQUIRE(s.last_errors().empty());
  CHECK(read_text(d.file()) == text);  // not rewritten at load
  REQUIRE(s.current().keys[0]);
  CHECK(s.current().keys[0]->settings["tap_action"] == "task_manager");  // cpu_load
  REQUIRE(s.current().keys[5]);
  CHECK(s.current().keys[5]->settings["tap_target"] == "ms-clock:");  // clock
  json page = s.document();
  page["device"]["brightness"] = 99;
  REQUIRE(s.update(page, 0).status == ld::UpdateStatus::Applied);
  page["device"]["brightness"] = 100;
  REQUIRE(s.update(page, 0).status == ld::UpdateStatus::Applied);
  CHECK(read_text(d.file()) == std::string(kRealN4ProConfig));  // the defaults written with the save
}

namespace {
std::vector<int> widths_of(const ld::Config& c) {
  std::vector<int> w;
  for (const auto& z : c.strip) w.push_back(z.width);
  return w;
}
const ld::ConfigShape kStrip600{"strip-600", "Strip 600", 10, 4, 600};
}  // namespace

TEST_CASE("on a 600 px strip device, the default config fits the strip and loads again without a rewrite") {
  Dir d("ld_svc_strip600_default");
  std::string written;
  {
    ld::ConfigService s(d.file(), ld::find_module, kStrip600);
    s.load();  // no file: the default, sized to the strip
    CHECK(s.last_errors().empty());
    CHECK(widths_of(s.current()) == std::vector<int>{322, 278});
    written = read_text(d.file());
    CHECK(json::parse(written)["device"]["model"] == "strip-600");
  }
  ld::ConfigService s(d.file(), ld::find_module, kStrip600);
  s.load();
  CHECK(s.last_errors().empty());               // not the default layout because of the strip
  CHECK(read_text(d.file()) == written);         // the model is recorded once, never again
  CHECK(widths_of(s.current()) == std::vector<int>{322, 278});
  CHECK(s.update(s.document(), 0).status == ld::UpdateStatus::Unchanged);  // the page's PUT: no 422
}

TEST_CASE("an existing N4 Pro config moved to a 600 px strip device is fitted once, and fitted back on the N4 Pro") {
  Dir d("ld_svc_strip600_n4");
  write_text(d.file(), kRealN4ProConfig);  // zones 400 + 400
  std::string written;
  {
    ld::ConfigService s(d.file(), ld::find_module, kStrip600);
    s.load();
    REQUIRE(s.last_errors().empty());
    CHECK(widths_of(s.current()) == std::vector<int>{300, 300});
    REQUIRE(s.current().keys[0]);
    CHECK(s.current().keys[0]->module == "cpu_load");  // the layout is kept, not replaced by the default
    REQUIRE(s.current().strip[0].slot);
    CHECK(s.current().strip[0].slot->module == "now_playing");
    written = read_text(d.file());
    const json file = json::parse(written);
    CHECK(file["device"]["model"] == "strip-600");
    CHECK(file["strip"][0]["width"] == 300);
    CHECK(s.update(s.document(), 0).status == ld::UpdateStatus::Unchanged);
  }
  {
    ld::ConfigService s(d.file(), ld::find_module, kStrip600);  // idempotent: no second write
    s.load();
    CHECK(s.last_errors().empty());
    CHECK(read_text(d.file()) == written);
  }
  ld::ConfigService n4(d.file(), ld::find_module);  // back on the N4 Pro
  n4.load();
  CHECK(n4.last_errors().empty());
  CHECK(widths_of(n4.current()) == std::vector<int>{400, 400});
  CHECK(read_text(d.file()) == std::string(kRealN4ProConfig));  // the same file as before the trip
}

TEST_CASE("reload fills a short list up to the device") {
  Dir d("ld_svc_reload_fit");
  write_text(d.file(), doc_with_city("A").dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();
  json doc = doc_with_city("B");
  doc["keys"].erase(9);
  doc["knobs"] = {nullptr};
  write_text(d.file(), doc.dump());
  s.reload();
  CHECK(city_of(s) == "B");
  CHECK(s.current().keys.size() == 10);
  CHECK(s.current().knobs.size() == 4);
  CHECK(s.current().ring_colors.size() == 4);
}

TEST_CASE("seconds below: a config without it loads with seconds unchanged and saves it off; on, it round-trips") {
  json doc = ld::to_json(ld::default_config());
  int clock = -1, stopwatch = -1;
  for (int i = 0; i < static_cast<int>(doc["keys"].size()); ++i) {
    if (!doc["keys"][i].is_object()) continue;
    if (doc["keys"][i]["module"] == "clock") clock = i;
    if (doc["keys"][i]["module"] == "stopwatch") stopwatch = i;
  }
  REQUIRE(clock >= 0);
  REQUIRE(stopwatch >= 0);
  for (bool seconds : {true, false}) {
    INFO(seconds);
    json old = doc;
    old["keys"][clock]["settings"] = {{"seconds", seconds}, {"tap_action", "open"}, {"tap_target", "ms-clock:"}};
    old["keys"][stopwatch]["settings"] = {{"file", ""}};
    const auto parsed = ld::parse_config(old, ld::find_module);
    REQUIRE(parsed.errors.empty());
    REQUIRE(parsed.config);
    const json saved = ld::to_json(*parsed.config);
    CHECK(saved["keys"][clock]["settings"] ==
          json{{"seconds", seconds}, {"seconds_below", false}, {"tap_action", "open"}, {"tap_target", "ms-clock:"}});
    CHECK(saved["keys"][stopwatch]["settings"] == json{{"file", ""}, {"seconds_below", false}});
  }
  json on = doc;
  on["keys"][clock]["settings"]["seconds_below"] = true;
  on["keys"][stopwatch]["settings"]["seconds_below"] = true;
  const auto parsed = ld::parse_config(on, ld::find_module);
  REQUIRE(parsed.config);
  CHECK(ld::to_json(*parsed.config) == on);
}

TEST_CASE("ConfigService::update refuses unknown keys growing beyond 4 KB, keeps those already in the file, and still reads them") {
  Dir d("ld_svc_extra");
  json big = doc_with_city("A");
  big["notes"] = std::string(5000, 'x');
  write_text(d.file(), big.dump());
  ld::ConfigService s(d.file(), city_lookup);
  s.load();  // a hand-written file keeps all its keys
  CHECK(s.document()["notes"].get<std::string>().size() == 5000);
  json doc = doc_with_city("B");
  doc["notes"] = std::string(5001, 'x');  // more than the file already holds: refused
  const ld::UpdateResult refused = s.update(doc, 0);
  CHECK(refused.status == ld::UpdateStatus::Invalid);
  REQUIRE(refused.errors.size() == 1);
  CHECK(refused.errors[0].message.find("unknown keys") != std::string::npos);
  CHECK(city_of(s) == "A");
  CHECK(read_text(d.file()) == big.dump());  // the hand-written file is left as it is
  doc["notes"] = std::string(5000, 'x');  // the page sends back what it was given: saved
  CHECK(s.update(doc, 0).status == ld::UpdateStatus::Applied);
  CHECK(city_of(s) == "B");
  doc["notes"] = "short";
  CHECK(s.update(doc, 0).status == ld::UpdateStatus::Applied);
  CHECK(s.document()["notes"] == "short");
  doc["notes"] = std::string(5000, 'x');  // once gone, over 4 KB again is growth
  CHECK(s.update(doc, 0).status == ld::UpdateStatus::Invalid);
}
