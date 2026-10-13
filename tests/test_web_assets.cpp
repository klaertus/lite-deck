#include <doctest/doctest.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "core/types.h"
#include "render/look.h"
#include "render/look_json.h"
#include "web/assets.h"
#include "web_test_util.h"

namespace {

constexpr const char* kScripts[] = {"/elements.js", "/appearance.js", "/settings.js", "/deck.js", "/app.js"};

std::string_view asset(const char* path) {
  auto a = ld::embedded_asset(path);
  REQUIRE(a);
  return a->data;
}

bool has(std::string_view text, std::string_view what) { return text.find(what) != std::string_view::npos; }

std::size_t lines(std::string_view text) { return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')); }

// The files web.rc embeds, by the path the page asks for ("/" for index.html).
std::vector<std::string> embedded_paths() {
  std::vector<std::string> out;
  std::istringstream rc(testutil::read_text(std::filesystem::path(LD_TEST_DIR) / ".." / "src" / "web" / "assets" / "web.rc"));
  std::string line;
  while (std::getline(rc, line)) {
    const auto q = line.find("RCDATA \"");
    if (q == std::string::npos) continue;
    const std::string file = line.substr(q + 8, line.find('"', q + 8) - (q + 8));
    out.push_back(file == "index.html" ? "/" : "/" + file);
  }
  return out;
}

// Bytes without carriage returns: the budget measures the content, whatever the checkout's line ends.
std::size_t content_size(std::string_view s) {
  return s.size() - static_cast<std::size_t>(std::count(s.begin(), s.end(), '\r'));
}

std::size_t occurrences(std::string_view text, std::string_view what) {
  std::size_t n = 0;
  for (auto i = text.find(what); i != std::string_view::npos; i = text.find(what, i + 1)) ++n;
  return n;
}

// The names a page script declares at top level (a line starting with function, async function,
// class, const, let or var): all page scripts share one global scope.
std::vector<std::string> top_level_names(std::string_view js) {
  std::vector<std::string> names;
  size_t pos = 0;
  while (pos < js.size()) {
    size_t end = js.find('\n', pos);
    if (end == std::string_view::npos) end = js.size();
    std::string_view line = js.substr(pos, end - pos);
    pos = end + 1;
    for (std::string_view kw : {"async function ", "function ", "class ", "const ", "let ", "var "}) {
      if (line.substr(0, kw.size()) != kw) continue;
      line.remove_prefix(kw.size());
      size_t n = 0;
      while (n < line.size() && (std::isalnum(static_cast<unsigned char>(line[n])) || line[n] == '_' || line[n] == '$'))
        ++n;
      if (n > 0) names.emplace_back(line.substr(0, n));
      break;
    }
  }
  return names;
}

}  // namespace

TEST_CASE("the page, its scripts and its stylesheet are embedded in the executable, in order") {
  const std::string_view page = asset("/");
  CHECK(ld::embedded_asset("/")->content_type == "text/html; charset=utf-8");
  CHECK(has(page, "href=\"/app.css\""));
  CHECK(ld::embedded_asset("/app.css")->content_type == "text/css; charset=utf-8");
  std::size_t last = 0;
  for (const char* path : kScripts) {
    INFO(path);
    CHECK(ld::embedded_asset(path)->content_type == "text/javascript; charset=utf-8");
    const auto at = page.find(std::string("<script src=\"") + path + "\" defer></script>");
    REQUIRE(at != std::string_view::npos);
    CHECK(at > last);  // app.js last: it starts the page once the others have declared their names
    last = at;
  }
  CHECK(has(page, "<datalist id=\"font-list\"></datalist>"));
  CHECK(has(asset("/app.js"), "new EventSource('/api/events')"));
  CHECK_FALSE(ld::embedded_asset("/index.html"));
  CHECK_FALSE(ld::embedded_asset("/../config.json"));
  CHECK_FALSE(ld::embedded_asset("/app.js/"));
}

TEST_CASE("the page needs no inline script or style (strict CSP), and its text is never markup") {
  const std::string_view page = asset("/");
  CHECK_FALSE(has(page, "<script>"));
  CHECK_FALSE(has(page, " style="));
  CHECK_FALSE(has(page, " onclick="));
  for (const char* path : kScripts) {
    INFO(path);
    const std::string_view js = asset(path);
    CHECK_FALSE(has(js, "innerHTML"));
    CHECK_FALSE(has(js, "outerHTML"));
    CHECK_FALSE(has(js, "insertAdjacentHTML"));
    CHECK_FALSE(has(js, "setAttribute('style'"));
    CHECK(lines(js) <= 800);
  }
}

TEST_CASE("the embedded files keep their UTF-8 text") {
  CHECK(has(asset("/app.js"), "Session expired"));
  CHECK(has(asset("/app.js"), "\xE2\x80\xA6"));         // the ellipsis of "Configure…"
  CHECK(has(asset("/appearance.js"), "Screen brightness"));
  CHECK(has(asset("/settings.js"), "\xE2\x80\xBA"));    // the › of the breadcrumb
}

TEST_CASE("each embedded file's ETag is worked out once, from its bytes") {
  for (const char* path : {"/", "/app.css", "/app.js", "/appearance.js", "/elements.js", "/settings.js", "/deck.js"}) {
    CAPTURE(path);
    const auto a = ld::embedded_asset(path);
    REQUIRE(a);
    CHECK(a->etag == ld::asset_etag(a->data));
    CHECK(ld::embedded_asset(path)->etag.data() == a->etag.data());  // the same kept string, not hashed again
  }
}

TEST_CASE("no two page scripts declare the same top-level name (one global scope)") {
  std::map<std::string, std::string> owner;
  for (const char* path : kScripts) {
    const auto names = top_level_names(asset(path));
    CHECK(names.size() > 3);  // the scan finds the declarations
    for (const std::string& name : names) {
      INFO(path << " declares " << name);
      auto [it, fresh] = owner.emplace(name, path);
      CHECK((fresh || it->second == path));
    }
  }
  CHECK(owner.at("edit") == "/app.js");
  CHECK(owner.at("buildDeck") == "/deck.js");
  CHECK(owner.at("slotPanel") == "/settings.js");
  CHECK(owner.at("slotLook") == "/elements.js");
  CHECK(owner.at("lookPanel") == "/appearance.js");
}

TEST_CASE("the page stays within its budget: 150 KB in all, 115 KB of script, 24 KB of style, 8 KB of icons, 7 requests") {
  const std::vector<std::string> paths = embedded_paths();
  REQUIRE(paths.size() >= 7);
  std::size_t total = 0, js = 0, css = 0;
  for (const std::string& p : paths) {
    INFO(p);
    auto a = ld::embedded_asset(p);
    REQUIRE(a);  // web.rc and assets.cpp list the same files
    const std::size_t n = content_size(a->data);
    total += n;
    if (p.ends_with(".js")) js += n;
    if (p.ends_with(".css")) css += n;
    if (p.ends_with(".js")) {
      CHECK(a->data.find("eval(") == std::string_view::npos);
      CHECK(a->data.find("new Function") == std::string_view::npos);
      CHECK(a->data.find("setInterval(") == std::string_view::npos);  // nothing runs on its own
    }
    if (p.ends_with(".css")) CHECK(a->data.find("@font-face") == std::string_view::npos);  // system fonts only
  }
  MESSAGE("page: " << total << " B, script: " << js << " B, style: " << css << " B");
  CHECK(total <= 150 * 1024);
  CHECK(js <= 115 * 1024);
  CHECK(css <= 24 * 1024);
  const std::string_view page = asset("/");
  CHECK(1 + occurrences(page, "<script src=") + occurrences(page, "rel=\"stylesheet\"") <= 7);
  CHECK(occurrences(page, "src=\"http") + occurrences(page, "href=\"http") == 0);  // no CDN
  std::size_t svg = 0;
  for (auto i = page.find("<svg"); i != std::string_view::npos; i = page.find("<svg", i + 1)) {
    const auto end = page.find("</svg>", i);
    REQUIRE(end != std::string_view::npos);
    svg += end + 6 - i;
  }
  CHECK(svg <= 8 * 1024);
}

TEST_CASE("the page knows the daemon's text roles and status palette defaults") {
  const std::string_view look = asset("/appearance.js");
  CHECK(has(look, "function roleWith(roles, role, prop, value)"));
  CHECK(has(look, "'/api/fonts'"));
  CHECK(has(look, "Not installed on this PC"));
  for (ld::TextRole role : ld::kTextRoles) {
    INFO(ld::role_name(role));
    CHECK(has(look, "'" + std::string(ld::role_name(role)) + "'"));
  }
  const ld::StatePalette p;
  for (ld::Rgb c : {p.warning, p.critical, p.idle, p.stale}) {
    INFO(ld::format_color(c));
    CHECK(has(look, "'" + ld::format_color(c) + "'"));
  }
  const std::string_view parts = asset("/elements.js");
  CHECK(has(parts, "function withRoleDefaults(e, kind)"));
  CHECK(has(parts, "function rowsFor(info, e)"));
  CHECK(has(parts, "'gradient'"));
}

TEST_CASE("the page builds the deck from GET /api/device, and no zone without a strip") {
  const std::string_view app = asset("/app.js");
  const auto device = app.find("const dv = await api('GET', '/api/device');");
  REQUIRE(device != std::string_view::npos);
  const auto expired = app.find("if (dv.status === 401) { fatal(EXPIRED); return; }", device);
  const auto other = app.find("if (dv.status !== 200)", device);
  REQUIRE(expired != std::string_view::npos);
  CHECK(expired < other);  // the 401 is told apart before the generic failure
  const std::string_view deck = asset("/deck.js");
  CHECK(has(deck, "--cols"));
  CHECK(has(deck, "knob_rings"));
  CHECK(has(deck, "strip.hidden = !dv.strip;"));
  CHECK(has(deck, "if (state.device.strip) buildZones();"));
  CHECK_FALSE(has(deck, "STRIP_WIDTH"));
  const std::string_view css = asset("/app.css");
  CHECK(has(css, "repeat(var(--cols, 5), 1fr)"));
  CHECK(has(css, "aspect-ratio: var(--key-ratio, 1)"));
  CHECK(has(css, "[hidden] { display: none !important; }"));
}

TEST_CASE("the settings form masks secrets and shows hints and placeholders") {
  const std::string_view js = asset("/settings.js");
  CHECK(has(js, "f.type === 'secret'"));
  CHECK(has(js, "type: 'password'"));
  CHECK(has(js, "autocomplete: 'off'"));
  CHECK(has(js, "hint: f.hint"));
  CHECK(has(js, "placeholder: f.placeholder || ''"));
}

TEST_CASE("the page never loses a change unannounced: modules chosen by a click, a guard before leaving, focus kept") {
  const std::string_view app = asset("/app.js");
  CHECK(has(app, "if (outcome !== 'saved') state.dirty = true;"));
  CHECK(has(app, "window.addEventListener('beforeunload', onLeave)"));
  CHECK(has(app, "function keepFocus(render)"));
  const std::string_view settings = asset("/settings.js");
  CHECK_FALSE(has(settings, "onchange: (e) => setModule"));  // arrows on a closed list fire change
  CHECK(has(settings, "onclick: () => setModule(id)"));
  CHECK(has(asset("/deck.js"), "'data-f': `${kind}-${i}`"));
}

TEST_CASE("the page is dark only, with the deck's neutral greys and state colors as tokens") {
  const std::string_view css = asset("/app.css");
  CHECK(has(css, "color-scheme: dark;"));
  for (const char* token : {"--text: #f2f2f2", "--text-2: #8c8c8c", "--text-3: #5f5f5f", "--warning: #ffb000",
                            "--critical: #ff453a", "--stale: #5f5f5f", "--hue: #e6e6e6"}) {
    INFO(token);
    CHECK(has(css, token));
  }
  CHECK(has(css, ":focus-visible"));
  CHECK(has(css, "@media (forced-colors: active)"));
  CHECK(has(css, "@media (prefers-reduced-motion: reduce)"));
}

TEST_CASE("the page speaks the glossary: plain words on screen, never the config's or the code's") {
  for (const char* path : kScripts) {
    INFO(path);
    const std::string_view js = asset(path);
    for (const char* word : {"'Typography'", "Inherit", "'Idle'", "'Stale'", "'Element'", "slot concerned", "Rejected",
                             "Changing…", "daemon is not", "Daemon unreachable", "colour", "of this slot", "'+ Zone'",
                             "- Empty -", "Tap action"}) {
      INFO(word);
      CHECK_FALSE(has(js, word));
    }
  }
  const std::string_view look = asset("/appearance.js");
  for (const char* word : {"'No data'", "'Out of date'", "'Text styles'", "'Status colors'", "'Default font'"}) CHECK(has(look, word));
  const std::string_view parts = asset("/elements.js");
  for (const char* word : {"'Parts'", "'One color'", "'By status'", "'Color scale'", "'Upper line'"}) CHECK(has(parts, word));
  const std::string_view settings = asset("/settings.js");
  for (const char* word : {"'On tap'", "'Warning levels'", "'Nothing (empty)'", "'Split zone'", "'Merge into left'"})
    CHECK(has(settings, word));
  CHECK(has(asset("/app.js"), "'lite-deck not responding'"));
}
