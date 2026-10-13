#include <doctest/doctest.h>
#include <set>
#include <string>
#include "device/registry.h"
#include "fake_web_backend.h"
#include "modules/module.h"
#include "web/api.h"

using nlohmann::json;

namespace {
const std::string kToken = "0123456789abcdef0123456789abcdef";

std::vector<ld::ModuleInfo> test_modules() {
  ld::ModuleInfo m;
  m.id = "clock";
  m.name = "Clock";
  m.slots = {ld::SlotKind::Key, ld::SlotKind::StripZone};
  m.settings_schema = json::parse(R"({"fields":[{"key":"color","type":"color","default":"#ffffff","label":"Color"}]})");
  m.elements = {ld::ElementSpec{.key = "time", .label = "Time", .size = 27, .weight = 700, .min_size = 10,
                                .max_size = 60, .role = ld::TextRole::LongValue},
                ld::ElementSpec{"title", "Title", 32.48f, 600, 10, 60}, ld::ElementSpec{"icon", "Icon", 26, 400, 10, 80, true}};
  m.ring_states = {ld::RingStateSpec{"normal", "Normal", "color", "brightness"}};
  return {m};
}

struct Api {
  FakeWebBackend backend;
  std::vector<ld::ModuleInfo> modules = test_modules();
  ld::WebApi api{kToken, backend, modules, [](std::string_view path) -> std::optional<ld::Asset> {
                   if (path == "/") return ld::Asset{"<!doctype html><title>t</title>", "text/html; charset=utf-8"};
                   return std::nullopt;
                 }, ld::default_device().layout};
  Api() { api.set_port(7878); }
  ld::WebRequest get(const std::string& path, bool authed = true) const {
    ld::WebRequest r;
    r.method = "GET";
    r.path = path;
    r.headers["host"] = "127.0.0.1:7878";
    if (authed) r.headers["cookie"] = "theme=dark; ld=" + kToken;
    return r;
  }
  ld::WebRequest put(const std::string& body) const {
    ld::WebRequest r = get("/api/config");
    r.method = "PUT";
    r.headers["x-lite-deck"] = "1";
    r.headers["content-type"] = "application/json";
    r.body = body;
    return r;
  }
};

std::string header_of(const ld::WebResponse& r, const std::string& name) {
  for (const auto& [k, v] : r.headers)
    if (k == name) return v;
  return {};
}

json body_of(const ld::WebResponse& r) { return json::parse(r.body); }
}  // namespace

TEST_CASE("GET /api/fonts lists the installed families; other methods get 405, no cookie 401") {
  Api a;
  ld::WebResponse r = a.api.handle(a.get("/api/fonts"));
  REQUIRE(r.status == 200);
  CHECK(r.content_type == "application/json; charset=utf-8");
  CHECK(body_of(r) == json::array({"Arial", "Segoe UI"}));
  ld::WebRequest put = a.put("{}");
  put.path = "/api/fonts";
  CHECK(a.api.handle(put).status == 405);
  CHECK(a.api.handle(a.get("/api/fonts", false)).status == 401);
}

TEST_CASE("GET /api/modules lists each module's elements") {
  Api a;
  json m = body_of(a.api.handle(a.get("/api/modules")));
  const json& els = m[0]["elements"];
  REQUIRE(els.size() == 3);
  CHECK(els[0]["key"] == "time");
  CHECK(els[0]["label"] == "Time");
  CHECK(els[0]["size"] == 27.0);
  CHECK(els[0]["weight"] == 700);
  CHECK(els[0]["min"] == 10);
  CHECK(els[0]["max"] == 60);
  CHECK(els[0]["icon"] == false);
  CHECK(els[1]["size"] == 32.5);  // shown to the half pixel, like the format cache
  CHECK(els[2]["icon"] == true);
}

TEST_CASE("the link with the token sets the session cookie and redirects to the page") {
  Api a;
  ld::WebRequest req = a.get("/", false);
  req.query["t"] = kToken;
  ld::WebResponse r = a.api.handle(req);
  CHECK(r.status == 303);
  CHECK(header_of(r, "Location") == "/");
  CHECK(header_of(r, "Set-Cookie") == "ld=" + kToken + "; HttpOnly; SameSite=Strict; Path=/");
}

TEST_CASE("a wrong token or a missing cookie gets 401") {
  Api a;
  ld::WebRequest wrong = a.get("/", false);
  wrong.query["t"] = "wrong";
  ld::WebResponse r = a.api.handle(wrong);
  CHECK(r.status == 401);
  CHECK(r.content_type.starts_with("text/html"));
  CHECK(header_of(r, "Set-Cookie").empty());
  CHECK(a.api.handle(a.get("/", false)).status == 401);
  ld::WebResponse api = a.api.handle(a.get("/api/config", false));
  CHECK(api.status == 401);
  CHECK(api.content_type.starts_with("application/json"));
  ld::WebRequest stale = a.get("/api/config", false);
  stale.headers["cookie"] = "ld=" + std::string(32, '0');
  CHECK(a.api.handle(stale).status == 401);
}

TEST_CASE("a foreign Host header is refused with 403, even with a valid cookie") {
  Api a;
  for (const char* host : {"evil.example:7878", "127.0.0.1:7879", "127.0.0.1", ""}) {
    ld::WebRequest req = a.get("/api/config");
    req.headers["host"] = host;
    CHECK(a.api.handle(req).status == 403);
  }
  ld::WebRequest local = a.get("/api/config");
  local.headers["host"] = "localhost:7878";
  CHECK(a.api.handle(local).status == 200);
  ld::WebRequest rebound = a.get("/", false);  // DNS rebinding: no cookie for another name
  rebound.headers["host"] = "evil.example:7878";
  rebound.query["t"] = kToken;
  CHECK(a.api.handle(rebound).status == 403);
}

TEST_CASE("guard() applies the same Host and cookie rules as handle()") {
  Api a;
  CHECK_FALSE(a.api.guard(a.get("/api/events")));
  CHECK(a.api.guard(a.get("/api/events", false))->status == 401);
  ld::WebRequest evil = a.get("/api/events");
  evil.headers["host"] = "evil.example:7878";
  CHECK(a.api.guard(evil)->status == 403);
}

TEST_CASE("GET /api/modules lists the modules with slot names and schemas") {
  Api a;
  ld::WebResponse r = a.api.handle(a.get("/api/modules"));
  REQUIRE(r.status == 200);
  json m = body_of(r);
  REQUIRE(m.size() == 1);
  CHECK(m[0]["id"] == "clock");
  CHECK(m[0]["name"] == "Clock");
  CHECK(m[0]["slots"] == json::array({"key", "zone"}));
  CHECK(m[0]["schema"]["fields"][0]["type"] == "color");
}

TEST_CASE("GET /api/config returns the document with its revision as ETag") {
  Api a;
  a.backend.snapshot = ld::ConfigSnapshot{json{{"version", 1}, {"web", {{"port", 7878}}}}, 12};
  ld::WebResponse r = a.api.handle(a.get("/api/config"));
  CHECK(r.status == 200);
  CHECK(header_of(r, "ETag") == "\"12\"");
  CHECK(header_of(r, "Cache-Control") == "no-store");
  CHECK(body_of(r)["web"]["port"] == 7878);
}

TEST_CASE("GET /api/status passes the backend's state through") {
  Api a;
  ld::WebResponse r = a.api.handle(a.get("/api/status"));
  CHECK(r.status == 200);
  CHECK(body_of(r)["device"] == "connected");
}

TEST_CASE("PUT /api/config requires X-Lite-Deck and a JSON content type") {
  Api a;
  ld::WebRequest no_header = a.put("{}");
  no_header.headers.erase("x-lite-deck");
  CHECK(a.api.handle(no_header).status == 403);
  ld::WebRequest form = a.put("{}");
  form.headers["content-type"] = "application/x-www-form-urlencoded";
  CHECK(a.api.handle(form).status == 415);
  CHECK(a.backend.update_calls == 0);
  ld::WebRequest charset = a.put("{}");
  charset.headers["content-type"] = "application/json; charset=utf-8";
  CHECK(a.api.handle(charset).status == 200);
  CHECK(a.backend.update_calls == 1);
}

TEST_CASE("PUT /api/config forwards If-Match and returns the configuration in force") {
  Api a;
  ld::WebRequest req = a.put(R"({"version":1,"x":2})");
  req.headers["if-match"] = "\"4\"";
  ld::WebResponse r = a.api.handle(req);
  CHECK(r.status == 200);
  CHECK(a.backend.last_expected == 4);
  CHECK(header_of(r, "ETag") == "\"5\"");
  json b = body_of(r);
  CHECK(b["revision"] == 5);
  CHECK(b["config"]["x"] == 2);
  a.api.handle(a.put("{}"));  // no If-Match: no revision check
  CHECK(a.backend.last_expected == 0);
}

TEST_CASE("PUT /api/config maps validation errors to 422 with every path") {
  Api a;
  a.backend.next_update = ld::UpdateResult{
      ld::UpdateStatus::Invalid,
      {{"strip", "widths must add up to 800 (currently 700)"}, {"keys[0].settings", "city: text expected"}},
      4};
  ld::WebResponse r = a.api.handle(a.put("{}"));
  CHECK(r.status == 422);
  json e = body_of(r)["errors"];
  REQUIRE(e.size() == 2);
  CHECK(e[0]["path"] == "strip");
  CHECK(e[1]["path"] == "keys[0].settings");
}

TEST_CASE("PUT /api/config maps a stale revision to 412, a write failure to 500, a silent loop to 503") {
  Api a;
  a.backend.next_update = ld::UpdateResult{ld::UpdateStatus::Conflict, {}, 9};
  ld::WebResponse conflict = a.api.handle(a.put("{}"));
  CHECK(conflict.status == 412);
  CHECK(header_of(conflict, "ETag") == "\"9\"");
  a.backend.next_update = ld::UpdateResult{ld::UpdateStatus::WriteFailed, {{"", "cannot write config.json"}}, 4};
  CHECK(a.api.handle(a.put("{}")).status == 500);
  a.backend.silent = true;
  CHECK(a.api.handle(a.put("{}")).status == 503);
  CHECK(a.api.handle(a.get("/api/config")).status == 503);
}

TEST_CASE("PUT /api/config rejects malformed JSON, invalid UTF-8 and oversized bodies before the loop") {
  Api a;
  CHECK(a.api.handle(a.put("{oops")).status == 400);
  CHECK(a.api.handle(a.put("{\"city\":\"\xff\"}")).status == 400);
  CHECK(a.api.handle(a.put(std::string(ld::WebApi::kMaxBody + 1, ' '))).status == 413);
  CHECK(a.backend.update_calls == 0);
}

TEST_CASE("PUT /api/config keeps non-ASCII settings intact") {
  Api a;
  const std::string city = "Liège - 東京";
  json doc{{"keys", json::array({json{{"module", "weather"}, {"settings", {{"city", city}}}}})}};
  CHECK(a.api.handle(a.put(doc.dump())).status == 200);
  CHECK(a.backend.last_doc["keys"][0]["settings"]["city"].get<std::string>() == city);
  json back = body_of(a.api.handle(a.get("/api/config")));
  CHECK(back["keys"][0]["settings"]["city"].get<std::string>() == city);
}

TEST_CASE("previews: keys 0-9 and the whole strip, nothing else") {
  Api a;
  ld::WebResponse k = a.api.handle(a.get("/api/preview/key/3"));
  CHECK(k.status == 200);
  CHECK(k.content_type == "image/jpeg");
  CHECK(k.body.size() == 4);
  CHECK(a.backend.last_preview == 3);
  CHECK(a.api.handle(a.get("/api/preview/strip/0")).status == 200);
  CHECK(a.backend.last_preview == -1);
  for (const char* bad : {"/api/preview/key/10", "/api/preview/key/x", "/api/preview/key/", "/api/preview/strip/1",
                          "/api/preview/knob/0"})
    CHECK(a.api.handle(a.get(bad)).status == 404);
  a.backend.jpeg.clear();
  CHECK(a.api.handle(a.get("/api/preview/key/0")).status == 503);
}

TEST_CASE("the page has a strict CSP; unknown paths are 404, wrong methods 405") {
  Api a;
  ld::WebResponse page = a.api.handle(a.get("/"));
  CHECK(page.status == 200);
  CHECK(page.content_type == "text/html; charset=utf-8");
  const std::string csp = header_of(page, "Content-Security-Policy");
  CHECK(csp.find("script-src 'self'") != std::string::npos);
  CHECK(csp.find("unsafe-inline") == std::string::npos);
  CHECK(header_of(page, "X-Content-Type-Options") == "nosniff");
  CHECK(a.api.handle(a.get("/nope")).status == 404);
  ld::WebRequest del = a.get("/api/config");
  del.method = "DELETE";
  CHECK(a.api.handle(del).status == 405);
  CHECK(a.api.handle(a.get("/favicon.ico")).status == 204);
}

TEST_CASE("If-Match: * means any revision, an unparseable value is 412") {
  Api a;
  ld::WebRequest star = a.put("{}");
  star.headers["if-match"] = "*";
  CHECK(a.api.handle(star).status == 200);
  CHECK(a.backend.last_expected == 0);
  for (const char* bad : {"abc", "\"x\"", "", "\"4\", \"5\"", "-1"}) {
    ld::WebRequest req = a.put("{}");
    req.headers["if-match"] = bad;
    CHECK(a.api.handle(req).status == 412);
  }
  CHECK(a.backend.update_calls == 1);
}

TEST_CASE("PUT /api/config caps the JSON nesting depth at 64") {
  Api a;
  auto nested = [](int levels) { return std::string(levels, '[') + std::string(levels, ']'); };
  CHECK(a.api.handle(a.put(nested(64))).status == 200);
  CHECK(a.api.handle(a.put(nested(65))).status == 400);
  CHECK(a.api.handle(a.put(nested(30000))).status == 400);  // 60 KB: under the size limit, refused for its depth
  CHECK(a.backend.update_calls == 1);
}

TEST_CASE("odd paths without a cookie are all 401") {
  Api a;
  for (const char* path : {"/api/../config", "/API/config", "/api/config/"}) CHECK(a.api.handle(a.get(path, false)).status == 401);
}

TEST_CASE("HEAD with the token link sets the cookie too") {
  Api a;
  ld::WebRequest req = a.get("/", false);
  req.method = "HEAD";
  req.query["t"] = kToken;
  ld::WebResponse r = a.api.handle(req);
  CHECK(r.status == 303);
  CHECK(header_of(r, "Set-Cookie") == "ld=" + kToken + "; HttpOnly; SameSite=Strict; Path=/");
}

TEST_CASE("the login page is in English and the security headers are exposed for other responses") {
  Api a;
  ld::WebResponse r = a.api.handle(a.get("/", false));
  CHECK(r.body.find("no longer valid") != std::string::npos);
  CHECK(header_of(ld::with_security_headers(ld::WebResponse{}), "X-Frame-Options") == "DENY");
}

TEST_CASE("GET /api/modules describes each element's kind, row, alignment, colour and visibility, and the ring states") {
  Api a;
  json m = body_of(a.api.handle(a.get("/api/modules")));
  const json& els = m[0]["elements"];
  CHECK(els[0]["kind"] == "text");
  CHECK(els[0]["row"] == "middle");
  CHECK(els[0]["align"] == "center");
  CHECK(els[0]["color_mode"] == "fixed");
  CHECK(els[0]["color"] == "#ffffff");
  CHECK(els[0]["visible"] == true);
  CHECK(els[0]["gradient"] == false);
  CHECK(els[2]["kind"] == "icon");  // declared with icon = true
  CHECK(m[0]["two_line"] == false);
  CHECK(m[0]["ring_states"] == json::array({json{{"key", "normal"}, {"label", "Normal"}, {"color", "color"},
                                                 {"brightness", "brightness"}}}));
}

TEST_CASE("the real modules: two-line keys, graphs and ring states are described") {
  const json all = ld::modules_to_json(ld::module_registry());
  auto by_id = [&](const char* id) {
    for (const json& m : all)
      if (m["id"] == id) return m;
    return json();
  };
  CHECK(by_id("crypto_gold")["two_line"] == true);
  CHECK(by_id("ram")["two_line"] == false);
  bool graph = false;
  const json cpu = by_id("cpu_load");  // a local: a range-for over a temporary's member dangles
  for (const json& e : cpu["elements"]) graph = graph || e["kind"] == "graph";
  CHECK(graph);
  CHECK(by_id("seek")["ring_states"].size() == 2);
  CHECK(by_id("weather")["elements"][1]["gradient"] == true);
}

TEST_CASE("GET /api/modules gives each key text its role, and no role to the others") {
  Api a;
  json m = body_of(a.api.handle(a.get("/api/modules")));
  const json& els = m[0]["elements"];
  CHECK(els[0]["role"] == "long_value");
  CHECK_FALSE(els[1].contains("role"));
  CHECK_FALSE(els[2].contains("role"));
  const json all = ld::modules_to_json(ld::module_registry());
  int checked = 0;
  for (const json& mod : all) {
    for (const json& e : mod["elements"]) {
      if (mod["id"] == "cpu_load" && e["key"] == "value") { CHECK(e["role"] == "value"); ++checked; }
      if (mod["id"] == "cpu_load" && e["key"] == "graph") { CHECK_FALSE(e.contains("role")); ++checked; }
      if (mod["id"] == "crypto_gold" && e["key"] == "line2") { CHECK(e["role"] == "two_line"); ++checked; }
      if (mod["id"] == "now_playing") { CHECK_FALSE(e.contains("role")); ++checked; }
    }
  }
  CHECK(checked > 3);
}

TEST_CASE("GET /api/device describes the active device; other methods get 405, no cookie 401") {
  Api a;  // the default device: the N4 Pro
  const ld::WebResponse r = a.api.handle(a.get("/api/device"));
  REQUIRE(r.status == 200);
  CHECK(body_of(r) == json::parse(R"({"id":"mirabox-n4pro","name":"MiraBox N4 Pro E","rows":2,"cols":5,
    "key_size":{"width":112,"height":112},"strip":{"width":800,"height":112},"knobs":4,"knob_rings":true,
    "vendor_app":"StreamDock"})"));
  ld::WebRequest put = a.get("/api/device");
  put.method = "PUT";
  CHECK(a.api.handle(put).status == 405);
  CHECK(a.api.handle(a.get("/api/device", false)).status == 401);
}

TEST_CASE("every module describes itself for the catalogue: a short name, one line, one of six categories, a hue") {
  static const std::set<std::string> categories{"System", "Time", "Weather & sky", "Media & sound", "Actions", "Online data"};
  std::set<std::string> used;
  for (const json& m : ld::modules_to_json(ld::module_registry())) {
    INFO(m["id"]);
    const std::string name = m["name"], description = m["description"], category = m["category"], hue = m["hue"];
    CHECK(name.size() <= 20);                    // short: the description says the rest
    CHECK_FALSE(description.empty());
    CHECK(description.size() <= 72);             // bytes: one line under the name
    CHECK(categories.contains(category));
    CHECK((hue.empty() || ld::parse_color(hue)));
    used.insert(category);
  }
  CHECK(used == categories);
  Api a;  // a module without them (the test one) still gets the fields, empty
  const json m = body_of(a.api.handle(a.get("/api/modules")))[0];
  CHECK(m["description"] == "");
  CHECK(m["category"] == "");
  CHECK(m["hue"] == "");
}

TEST_CASE("GET /api/modules keeps options as values and gives labelled ones in option_labels") {
  const json s = ld::public_schema(json::parse(R"({"fields":[
    {"key":"a","type":"enum","default":"x","options":[{"value":"x","label":"Ex"},{"value":"y"}],"label":"A"},
    {"key":"b","type":"enum","default":"p","options":["p","q"],"label":"B"}]})"));
  CHECK(s["fields"][0]["options"] == json::array({"x", "y"}));
  CHECK(s["fields"][0]["option_labels"] == json::array({"Ex", "y"}));
  CHECK(s["fields"][1]["options"] == json::array({"p", "q"}));
  CHECK_FALSE(s["fields"][1].contains("option_labels"));
}

TEST_CASE("the real modules speak plainly: short labels, labelled options, the glossary's words") {
  for (const json& m : ld::modules_to_json(ld::module_registry())) {
    for (const json& f : m["schema"]["fields"]) {
      INFO(m["id"] << "." << f["key"]);
      const std::string label = f["label"];
      CHECK(label.size() <= 32);  // help goes to the hint, not the label
      for (const char* jargon : {"Hysteresis", "Gradient", "colour", "warning from", "critical from", "(empty =", "(0 ="})
        CHECK(label.find(jargon) == std::string::npos);
      if (f["type"] == "enum") CHECK(f.contains("option_labels"));  // never a raw value such as task_manager
    }
  }
}

TEST_CASE("previews carry the backend's ETag; the same tag again gets 304 without asking for the frame") {
  Api a;
  a.backend.tag = "\"1-7\"";
  ld::WebResponse first = a.api.handle(a.get("/api/preview/key/2"));
  CHECK(first.status == 200);
  CHECK(header_of(first, "ETag") == "\"1-7\"");
  CHECK(a.backend.last_preview == 2);
  a.backend.last_preview = -99;
  ld::WebRequest again = a.get("/api/preview/key/2");
  again.headers["if-none-match"] = "\"1-7\"";
  ld::WebResponse same = a.api.handle(again);
  CHECK(same.status == 304);
  CHECK(same.body.empty());
  CHECK(header_of(same, "ETag") == "\"1-7\"");
  CHECK(a.backend.last_preview == -99);  // no frame asked for
  again.headers["if-none-match"] = "\"1-6\"";
  CHECK(a.api.handle(again).status == 200);
  a.backend.tag.clear();  // no tag: always the frame, never a 304
  CHECK(a.api.handle(again).status == 200);
}

TEST_CASE("the page may show previews from blob: URLs (fetched with their ETag), nothing else is loosened") {
  Api a;
  const std::string csp = header_of(a.api.handle(a.get("/")), "Content-Security-Policy");
  CHECK(csp.find("img-src 'self' blob:;") != std::string::npos);
  CHECK(csp.find("script-src 'self';") != std::string::npos);
  CHECK(csp.find("style-src 'self';") != std::string::npos);
}

TEST_CASE("the page's files carry an ETag and are checked at each load: 304 while unchanged, the API stays no-store") {
  Api a;
  ld::WebResponse page = a.api.handle(a.get("/"));
  REQUIRE(page.status == 200);
  const std::string tag = header_of(page, "ETag");
  CHECK(tag.size() == 18);  // "<16 hex digits>"
  CHECK(header_of(page, "Cache-Control") == "no-cache");
  CHECK_FALSE(header_of(page, "Content-Security-Policy").empty());
  ld::WebRequest again = a.get("/");
  again.headers["if-none-match"] = tag;
  ld::WebResponse same = a.api.handle(again);
  CHECK(same.status == 304);
  CHECK(same.body.empty());
  CHECK(header_of(same, "ETag") == tag);
  again.headers["if-none-match"] = "\"0000000000000000\"";
  CHECK(a.api.handle(again).status == 200);
  ld::WebResponse api = a.api.handle(a.get("/api/config"));
  CHECK(header_of(api, "Cache-Control") == "no-store");
  int cache_headers = 0;
  for (const auto& [k, v] : page.headers) cache_headers += k == "Cache-Control";
  CHECK(cache_headers == 1);
}

TEST_CASE("a fully customised N4 Pro configuration fits three times in the 64 KB PUT limit") {
  json keys = json::array();
  for (int i = 0; i < 10; ++i) {
    json settings = json::object();
    for (int s = 0; s < 14; ++s) settings["setting_" + std::to_string(s)] = std::string(80, 'x');
    json elements = json::object();
    for (const char* e : {"label", "value", "icon"})
      elements[e] = {{"size", 30}, {"weight", 700}, {"row", "middle"}, {"align", "center"}, {"color", "#ffffff"}, {"visible", true}};
    keys.push_back({{"module", "launcher"}, {"settings", settings}, {"elements", elements}, {"font", "Bahnschrift SemiBold"}, {"italic", false}});
  }
  json strip = json::array();
  for (int z = 0; z < 8; ++z) strip.push_back({{"width", 100}, {"module", "now_playing"}, {"settings", {{"scroll_speed", 2}, {"color", "#ffffff"}}}});
  json roles = json::object();
  for (const char* r : {"label", "value", "long_value", "two_line", "secondary", "symbol"}) roles[r] = {{"font", "Bahnschrift"}, {"size", 30}, {"weight", 700}};
  const json doc{{"version", 1}, {"keys", keys}, {"knobs", json::array({nullptr, nullptr, nullptr, nullptr})}, {"strip", strip},
                 {"appearance", {{"font", "Segoe UI"}, {"italic", false}, {"roles", roles}}}};
  CHECK(doc.dump().size() * 3 <= ld::WebApi::kMaxBody);
}

TEST_CASE("a page file's ETag follows its bytes, not the build") {
  FakeWebBackend backend;
  std::vector<ld::ModuleInfo> modules = test_modules();
  std::string body = "body { color: red }";
  ld::WebApi api{kToken, backend, modules, [&body](std::string_view path) -> std::optional<ld::Asset> {
                   if (path == "/style.css") return ld::Asset{body, "text/css; charset=utf-8"};
                   return std::nullopt;
                 }, ld::default_device().layout};
  api.set_port(7878);
  ld::WebRequest r;
  r.method = "GET";
  r.path = "/style.css";
  r.headers["host"] = "127.0.0.1:7878";
  r.headers["cookie"] = "ld=" + kToken;
  const std::string first = header_of(api.handle(r), "ETag");
  CHECK(header_of(api.handle(r), "ETag") == first);
  body = "body { color: blue }";
  ld::WebRequest stale = r;
  stale.headers["if-none-match"] = first;
  const ld::WebResponse changed = api.handle(stale);
  CHECK(changed.status == 200);
  CHECK(changed.body == body);
  CHECK(header_of(changed, "ETag") != first);
  CHECK(header_of(changed, "Cache-Control") == "no-cache");
}

TEST_CASE("If-None-Match is read as RFC 9110 says: a list, weak tags and * match, for page files and previews") {
  Api a;
  const std::string tag = header_of(a.api.handle(a.get("/")), "ETag");
  ld::WebRequest page = a.get("/");
  for (const std::string sent : {"\"0000000000000000\", " + tag, "W/" + tag, std::string("*"), " " + tag + " ,\"x\""}) {
    page.headers["if-none-match"] = sent;
    CHECK(a.api.handle(page).status == 304);
  }
  for (const std::string sent : {std::string("\"x\", \"y\""), tag.substr(1), std::string(""), std::string(",")}) {
    page.headers["if-none-match"] = sent;
    CHECK(a.api.handle(page).status == 200);
  }
  a.backend.tag = "\"1-7\"";
  ld::WebRequest preview = a.get("/api/preview/key/2");
  preview.headers["if-none-match"] = "\"1-6\", W/\"1-7\"";
  CHECK(a.api.handle(preview).status == 304);
  a.backend.tag.clear();  // no tag: * does not stand for a frame that has none
  preview.headers["if-none-match"] = "*";
  CHECK(a.api.handle(preview).status == 200);
}
