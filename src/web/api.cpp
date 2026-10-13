// src/web/api.cpp
#include "web/api.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include "render/look_json.h"
#include "web/auth.h"

namespace ld {
using nlohmann::json;

namespace {

constexpr int kMaxJsonDepth = 64;
constexpr const char* kJson = "application/json; charset=utf-8";
constexpr const char* kHtml = "text/html; charset=utf-8";
constexpr const char* kCsp =
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob:; connect-src 'self'; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
constexpr std::string_view kLoginPage =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>lite-deck</title></head><body>"
    "<h1>lite-deck</h1><p>This link is no longer valid. Open the configuration from the lite-deck icon "
    "in the notification area (left click, or right click then \"Configure…\").</p></body></html>";

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

WebResponse json_response(int status, const json& j) { return WebResponse{status, kJson, dump(j), {}}; }

WebResponse error_response(int status, const std::string& message) {
  return json_response(status, json{{"errors", json::array({json{{"path", ""}, {"message", message}}})}});
}

const std::string& header(const WebRequest& r, const std::string& lower_name) {
  static const std::string empty;
  auto it = r.headers.find(lower_name);
  return it == r.headers.end() ? empty : it->second;
}

std::string etag(std::uint64_t revision) { return "\"" + std::to_string(revision) + "\""; }

// If-None-Match against the current tag (RFC 9110 13.1.2): "*", or a comma-separated list of tags
// compared weakly (a W/ prefix is ignored). No current tag: nothing matches.
bool none_match(std::string_view sent, std::string_view tag) {
  if (tag.empty()) return false;
  while (!sent.empty()) {
    const std::size_t comma = sent.find(',');
    std::string_view one = sent.substr(0, comma);
    sent = comma == std::string_view::npos ? std::string_view() : sent.substr(comma + 1);
    while (!one.empty() && (one.front() == ' ' || one.front() == '\t')) one.remove_prefix(1);
    while (!one.empty() && (one.back() == ' ' || one.back() == '\t')) one.remove_suffix(1);
    if (one.starts_with("W/")) one.remove_prefix(2);
    if (one == "*" || one == tag) return true;
  }
  return false;
}

// Parses an If-Match value: "\"12\"", "W/\"12\"" or "12". Absent or "*" = no revision check (0).
// Present but unparseable = nullopt (the caller answers 412).
std::optional<std::uint64_t> parse_if_match(const WebRequest& r) {
  auto it = r.headers.find("if-match");
  if (it == r.headers.end()) return std::uint64_t{0};
  std::string_view v = it->second;
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
  if (v == "*") return std::uint64_t{0};
  if (v.starts_with("W/")) v.remove_prefix(2);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  std::uint64_t n = 0;
  auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  if (ec != std::errc{} || end != v.data() + v.size()) return std::nullopt;
  return n;
}

std::optional<int> parse_index(std::string_view s) {
  if (s.empty() || s.size() > 2) return std::nullopt;
  int n = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
  return n;
}

}  // namespace

WebResponse with_security_headers(WebResponse r) {
  r.headers.emplace_back("X-Content-Type-Options", "nosniff");
  r.headers.emplace_back("Referrer-Policy", "no-referrer");
  const bool cached = std::any_of(r.headers.begin(), r.headers.end(), [](const auto& h) { return h.first == "Cache-Control"; });
  if (!cached) r.headers.emplace_back("Cache-Control", "no-store");  // only the page's own files may be kept
  r.headers.emplace_back("X-Frame-Options", "DENY");
  if (r.content_type.starts_with("text/html")) r.headers.emplace_back("Content-Security-Policy", kCsp);
  return r;
}

std::string asset_etag(std::string_view data) {
  std::uint64_t h = 14695981039346656037ull;
  for (const unsigned char c : data) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return std::format("\"{:016x}\"", h);
}

namespace {

WebResponse finish(WebResponse r) { return with_security_headers(std::move(r)); }

// A file of the page: checked again at each load (no-cache), 304 while it has not changed.
WebResponse asset_response(const Asset& a, const std::string& if_none_match) {
  const std::string tag = a.etag.empty() ? asset_etag(a.data) : std::string(a.etag);
  const bool same = none_match(if_none_match, tag);
  WebResponse r{same ? 304 : 200, std::string(a.content_type), same ? std::string() : std::string(a.data), {}};
  r.headers.emplace_back("ETag", tag);
  r.headers.emplace_back("Cache-Control", "no-cache");
  return r;
}

const char* slot_name(SlotKind k) {
  return k == SlotKind::Key ? "key" : k == SlotKind::Knob ? "knob" : "zone";
}

}  // namespace

// A module's schema as GET /api/modules gives it: `options` stays a list of values, as before; the
// labels of {"value", "label"} options go to `option_labels`, in the same order.
json public_schema(const json& schema) {
  json out = schema;
  if (!out.is_object() || !out.contains("fields") || !out["fields"].is_array()) return out;
  for (json& f : out["fields"]) {
    if (!f.is_object() || !f.contains("options") || !f["options"].is_array()) continue;
    bool labelled = false;
    json values = json::array();
    json labels = json::array();
    for (const json& o : f["options"]) {
      const bool pair = o.is_object() && o.contains("value");
      labelled = labelled || pair;
      values.push_back(pair ? o["value"] : o);
      labels.push_back(pair ? o.value("label", o["value"]) : o);
    }
    if (!labelled) continue;
    f["options"] = std::move(values);
    f["option_labels"] = std::move(labels);
  }
  return out;
}

json modules_to_json(const std::vector<ModuleInfo>& modules) {
  json out = json::array();
  for (const auto& m : modules) {
    json slots = json::array();
    for (SlotKind k : m.slots) slots.push_back(slot_name(k));
    json elements = json::array();
    for (const ElementSpec& e : m.elements) {
      json j{{"key", e.key},
             {"label", e.label},
             {"size", std::round(static_cast<double>(e.size) * 2.0) / 2.0},
             {"weight", e.weight},
             {"min", e.min_size},
             {"max", e.max_size},
             {"icon", e.icon},
             {"kind", kind_name(e.icon ? ElementKind::Icon : e.kind)},
             {"row", row_name(e.row)},
             {"align", align_name(e.align)},
             {"color_mode", color_mode_name(e.color_mode)},
             {"color", format_color(e.color)},
             {"visible", e.visible},
             {"gradient", e.gradient}};
      if (e.role != TextRole::None) j["role"] = role_name(e.role);  // absent for None
      elements.push_back(std::move(j));
    }
    const bool two_line = std::any_of(m.elements.begin(), m.elements.end(), [](const ElementSpec& e) {
      return e.row == Row::Line1 || e.row == Row::Line2;
    });
    json rings = json::array();
    for (const RingStateSpec& r : m.ring_states)
      rings.push_back(json{{"key", r.key}, {"label", r.label}, {"color", r.color_key}, {"brightness", r.brightness_key}});
    out.push_back(json{{"id", m.id}, {"name", m.name}, {"description", m.description}, {"category", m.category},
                       {"hue", m.hue}, {"slots", slots}, {"schema", public_schema(m.settings_schema)},
                       {"elements", elements}, {"two_line", two_line}, {"ring_states", rings}});
  }
  return out;
}

json layout_to_json(const DeviceLayout& l) {
  json strip = nullptr;
  if (l.strip) strip = json{{"width", l.strip->w}, {"height", l.strip->h}};
  return json{{"id", l.id},
              {"name", l.name},
              {"rows", l.rows},
              {"cols", l.cols},
              {"key_size", {{"width", l.key_size.w}, {"height", l.key_size.h}}},
              {"strip", strip},
              {"knobs", l.knobs},
              {"knob_rings", l.knob_rings},
              {"vendor_app", l.vendor_app.empty() ? json(nullptr) : json(vendor_app_label(l))}};
}

WebApi::WebApi(std::string token, WebBackend& backend, const std::vector<ModuleInfo>& modules, AssetLookup assets,
               const DeviceLayout& layout)
    : token_(std::move(token)), backend_(backend), modules_json_(dump(modules_to_json(modules))),
      assets_(std::move(assets)), device_json_(dump(layout_to_json(layout))), key_count_(layout.key_count()),
      has_strip_(layout.strip.has_value()) {}

std::optional<WebResponse> WebApi::guard(const WebRequest& req) const {
  if (!host_allowed(header(req, "host"), port_)) return finish(error_response(403, "host refused"));
  if (same_token(cookie_value(header(req, "cookie"), "ld"), token_)) return std::nullopt;
  if (req.path.starts_with("/api/")) return finish(error_response(401, "session missing or expired"));
  return finish(WebResponse{401, kHtml, std::string(kLoginPage), {}});
}

WebResponse WebApi::handle(const WebRequest& req) {
  const bool reading = req.method == "GET" || req.method == "HEAD";
  // The token link has no cookie yet, so it skips guard()'s cookie check; it still gets the Host check.
  if (reading && req.path == "/" && req.query.contains("t")) {
    if (!host_allowed(header(req, "host"), port_)) return finish(error_response(403, "host refused"));
    return finish(login(req));
  }
  if (auto deny = guard(req)) return *deny;  // Host, then cookie
  if (reading) {
    if (req.path == "/favicon.ico") return finish(WebResponse{204, "text/plain", "", {}});
    if (assets_)
      if (auto a = assets_(req.path)) return finish(asset_response(*a, header(req, "if-none-match")));
    if (req.path == "/api/modules") return finish(WebResponse{200, kJson, modules_json_, {}});
    if (req.path == "/api/device") return finish(WebResponse{200, kJson, device_json_, {}});
    if (req.path == "/api/config") return finish(get_config());
    if (req.path == "/api/status") return finish(get_status());
    if (req.path == "/api/fonts") return finish(json_response(200, backend_.fonts()));
    if (req.path.starts_with("/api/preview/")) return finish(get_preview(std::string_view(req.path).substr(13), req));
  }
  if (req.path == "/api/config" && req.method == "PUT") return finish(put_config(req));
  if (req.path == "/api/config" || req.path == "/api/modules" || req.path == "/api/status" || req.path == "/api/fonts" ||
      req.path == "/api/device")
    return finish(error_response(405, "method not allowed"));
  return finish(error_response(404, "not found"));
}

WebResponse WebApi::login(const WebRequest& req) const {
  if (!same_token(req.query.at("t"), token_)) return WebResponse{401, kHtml, std::string(kLoginPage), {}};
  WebResponse r{303, "text/plain; charset=utf-8", "", {}};
  r.headers.emplace_back("Set-Cookie", "ld=" + token_ + "; HttpOnly; SameSite=Strict; Path=/");
  r.headers.emplace_back("Location", "/");
  return r;
}

WebResponse WebApi::get_config() {
  auto snap = backend_.config();
  if (!snap) return error_response(503, "the daemon is not responding");
  WebResponse r = json_response(200, snap->document);
  r.headers.emplace_back("ETag", etag(snap->revision));
  return r;
}

WebResponse WebApi::get_status() {
  auto st = backend_.status();
  if (!st) return error_response(503, "the daemon is not responding");
  return json_response(200, *st);
}

WebResponse WebApi::put_config(const WebRequest& req) {
  if (header(req, "x-lite-deck") != "1") return error_response(403, "missing X-Lite-Deck header");
  if (!is_json_content_type(header(req, "content-type"))) return error_response(415, "Content-Type: application/json expected");
  if (req.body.size() > kMaxBody) return error_response(413, "configuration too large");
  const auto expected = parse_if_match(req);
  if (!expected) return error_response(412, "malformed If-Match header");
  bool too_deep = false;
  json doc = json::parse(
      req.body,
      [&too_deep](int depth, json::parse_event_t event, json&) {
        if ((event == json::parse_event_t::object_start || event == json::parse_event_t::array_start) && depth >= kMaxJsonDepth)
          too_deep = true;
        return true;
      },
      false);  // invalid UTF-8 is a parse error too
  if (too_deep) return error_response(400, "JSON nested too deeply");
  if (doc.is_discarded()) return error_response(400, "invalid JSON");
  auto r = backend_.update_config(std::move(doc), *expected);
  if (!r) return error_response(503, "the daemon is not responding");
  switch (r->result.status) {
    case UpdateStatus::Applied:
    case UpdateStatus::Unchanged: {
      WebResponse ok = json_response(200, json{{"revision", r->snapshot.revision}, {"config", r->snapshot.document}});
      ok.headers.emplace_back("ETag", etag(r->snapshot.revision));
      return ok;
    }
    case UpdateStatus::Invalid:
      return json_response(422, json{{"errors", errors_to_json(r->result.errors)}});
    case UpdateStatus::Conflict: {
      WebResponse conflict = error_response(412, "the configuration was changed elsewhere");
      conflict.headers.emplace_back("ETag", etag(r->result.revision));
      return conflict;
    }
    case UpdateStatus::WriteFailed:
      return json_response(500, json{{"errors", errors_to_json(r->result.errors)}});
  }
  return error_response(500, "internal error");
}

WebResponse WebApi::get_preview(std::string_view what, const WebRequest& req) {
  int target = -2;
  if (what.starts_with("key/")) {
    if (auto i = parse_index(what.substr(4)); i && *i >= 0 && *i < key_count_) target = *i;
  } else if (what == "strip/0" && has_strip_) {
    target = -1;
  }
  if (target == -2) return error_response(404, "unknown preview");
  // Read before the frame is encoded: a newer frame labelled with this tag is only fetched once more.
  const std::string tag = backend_.preview_tag(target);
  if (none_match(header(req, "if-none-match"), tag)) {
    WebResponse same{304, "image/jpeg", "", {}};
    same.headers.emplace_back("ETag", tag);
    return same;
  }
  auto jpeg = backend_.preview(target);
  if (!jpeg || jpeg->empty()) return error_response(503, "preview unavailable");
  WebResponse r{200, "image/jpeg", std::move(*jpeg), {}};
  if (!tag.empty()) r.headers.emplace_back("ETag", tag);
  return r;
}

}  // namespace ld
