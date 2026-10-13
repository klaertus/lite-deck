// src/web/api.h
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/config_service.h"
#include "device/device.h"
#include "modules/module.h"

namespace ld {

struct WebRequest {
  std::string method;                          // "GET", "HEAD", "PUT"...
  std::string path;                            // decoded, without the query string
  std::map<std::string, std::string> headers;  // names in lower case
  std::map<std::string, std::string> query;
  std::string body;
};

struct WebResponse {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct ConfigSnapshot {
  nlohmann::json document;
  std::uint64_t revision = 0;
};

struct ConfigUpdate {
  UpdateResult result;
  ConfigSnapshot snapshot;  // the configuration in force after the call
};

// What the API needs from the daemon. Called from web threads; the real implementation (LoopBackend)
// hops to the loop. nullopt = the loop did not answer in time.
class WebBackend {
public:
  virtual ~WebBackend() = default;
  virtual std::optional<ConfigSnapshot> config() = 0;
  virtual std::optional<ConfigUpdate> update_config(nlohmann::json doc, std::uint64_t expected_revision) = 0;
  virtual std::optional<std::string> preview(int target) = 0;  // key 0 .. key_count - 1, strip -1
  // The ETag of the frame preview(target) would give now ("" = none). Any thread, never the loop: a
  // page that already has this frame gets a 304 without the loop being asked anything.
  virtual std::string preview_tag(int /*target*/) { return {}; }
  virtual std::optional<nlohmann::json> status() = 0;
  // Installed font families (render/fonts.h). Any thread, never the loop: the list is built once.
  virtual std::vector<std::string> fonts() = 0;
};

struct Asset {
  std::string_view data;
  std::string_view content_type;
  std::string_view etag = {};  // asset_etag(data) worked out once by the lookup; "" = worked out at each request
};
// The ETag of a page file: FNV-1a 64 of its bytes, so that a new build gets a new one.
std::string asset_etag(std::string_view data);
using AssetLookup = std::function<std::optional<Asset>(std::string_view path)>;

// Adds the hardening headers every response carries (nosniff, no-referrer, no-store, DENY; CSP on HTML).
WebResponse with_security_headers(WebResponse r);

nlohmann::json modules_to_json(const std::vector<ModuleInfo>& modules);
// `schema` with labelled enum options split into `options` (values) and `option_labels`.
nlohmann::json public_schema(const nlohmann::json& schema);
// The active device's data sheet for the page: id, name, grid, key size, strip (null if none), knobs,
// the vendor app's name (null if none).
nlohmann::json layout_to_json(const DeviceLayout& layout);

// Every HTTP rule of the config page, independent of the socket library: Host check, token link,
// session cookie, CSRF header, routes and status codes. Thread-safe once set_port() was called.
class WebApi {
public:
  // A full N4 Pro configuration is about 5-20 KB: 64 KB keeps a 3x margin and caps what one PUT costs.
  static constexpr std::size_t kMaxBody = 64 * 1024;

  WebApi(std::string token, WebBackend& backend, const std::vector<ModuleInfo>& modules, AssetLookup assets,
         const DeviceLayout& layout);
  void set_port(int port) { port_ = port; }  // the bound port; before the server starts
  int port() const { return port_; }
  const std::string& token() const { return token_; }

  WebResponse handle(const WebRequest& req);
  // Host and cookie checks only (used by the event stream). nullopt = allowed.
  std::optional<WebResponse> guard(const WebRequest& req) const;

private:
  WebResponse login(const WebRequest& req) const;
  WebResponse get_config();
  WebResponse put_config(const WebRequest& req);
  WebResponse get_preview(std::string_view what, const WebRequest& req);
  WebResponse get_status();

  std::string token_;
  WebBackend& backend_;
  std::string modules_json_;
  AssetLookup assets_;
  std::string device_json_;
  int key_count_ = 0;
  bool has_strip_ = false;
  int port_ = 0;
};

}  // namespace ld
