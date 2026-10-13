// src/web/web_ui.h
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/config_service.h"
#include "core/loop_api.h"
#include "layout/deck.h"
#include "render/jpeg.h"
#include "web/api.h"
#include "web/assets.h"
#include "web/event_hub.h"
#include "web/loop_bridge.h"
#include "web/server.h"

namespace ld {

// WebBackend of the real daemon: every read or change hops to the loop through LoopBridge.
//
// Previews: each target (key, strip) has a frame generation, bumped by frame() while a page is open
// or a preview has been handed out since the last drop. A preview is encoded once per generation,
// at the device's quality, and kept for every page; its ETag is "<epoch>-<generation>". When the
// last page closes (or, with no page open, WebUi::kPreviewIdle after the last preview asked for),
// drop_previews() frees the cache and the encoder and changes the epoch, so nothing stays and no old
// ETag matches a newer frame.
class LoopBackend final : public WebBackend {
public:
  static constexpr int kKeyQuality = 85;    // the device's (device/n4pro/n4pro_device.h)
  static constexpr int kStripQuality = 80;
  static constexpr std::size_t kMaxPreviewCache = 256 * 1024;
  struct PreviewStats {
    std::size_t encodes = 0;  // since the start
    std::size_t bytes = 0;    // JPEG buffer bytes kept now (the cache; not the encoder's stream and band buffer)
    bool encoder = false;     // an encoder (WIC factory, kept stream, band buffer) is held now
  };

  // `device_state` runs on the loop: "connected", "busy", "disconnected" or "simulator".
  LoopBackend(LoopBridge& bridge, ConfigService& config, Deck& deck, std::function<std::string()> device_state);
  std::optional<ConfigSnapshot> config() override;
  std::optional<ConfigUpdate> update_config(nlohmann::json doc, std::uint64_t expected_revision) override;
  std::optional<std::string> preview(int target) override;
  std::string preview_tag(int target) override;
  std::optional<nlohmann::json> status() override;
  std::vector<std::string> fonts() override;  // the cached system list, straight from the web thread

  void frame(int target);  // loop thread, pages open or previews out: `target` has a new frame
  void drop_previews();    // loop thread: no page open any more
  PreviewStats preview_stats() const;  // loop thread
  // Loop thread: a preview (and its ETag) was handed out since the last drop_previews(). Its
  // generation must then follow the frames even with no page open (a script fetching previews),
  // or the cache and the ETag would keep an old frame.
  bool previews_out() const { return out_; }
  std::function<void()> on_preview;  // loop thread, at each preview asked of the loop

private:
  struct Cached {
    std::uint64_t gen = 0;
    std::vector<std::uint8_t> jpeg;
  };
  std::size_t slot(int target) const;  // keys 0..n-1, then the strip

  LoopBridge& bridge_;
  ConfigService& config_;
  Deck& deck_;
  std::function<std::string()> device_state_;
  std::size_t targets_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> gens_;  // per target; read by web threads
  std::atomic<std::uint64_t> epoch_{1};
  std::vector<Cached> cache_;          // loop thread
  std::optional<BandJpegEncoder> jpeg_;  // loop thread, only while previews are out
  std::size_t encodes_ = 0;
  bool out_ = false;                   // loop thread, see previews_out()
};

// The whole web interface, built in dependency order. Construct, start, publish and stop on the
// loop thread.
class WebUi {
public:
  WebUi(LoopApi& loop, ConfigService& config, Deck& deck, std::function<std::string()> device_state,
        AssetLookup assets = embedded_asset);
  ~WebUi();
  WebUi(const WebUi&) = delete;
  WebUi& operator=(const WebUi&) = delete;

  bool start(int port);  // false: port taken (logged); the daemon runs on without web UI
  void stop();           // bridge first, then event streams, listener and pool; idempotent
  bool running() const;
  // http://127.0.0.1:<port>/?t=<token> while running, "" otherwise. The tray opens it; a
  // second launch reaches it through AppWindow's kCmdConfigure.
  std::wstring config_url() const;

  // Loop thread: push changes to the open pages (no-ops when none is open).
  void config_changed();                          // ConfigService revision or errors changed
  void device_changed(const std::string& state);  // "connected", "busy", "disconnected", "simulator"
  void frame(int target);                         // from Deck::on_frame
  LoopBackend::PreviewStats preview_stats() const { return backend_.preview_stats(); }  // loop thread

  // With no page open, the previews are freed this long after the last one asked for.
  static constexpr Duration kPreviewIdle{60000};
  void set_preview_idle(Duration d) { preview_idle_ = d; }  // tests; loop thread

private:
  void preview_served();
  void arm_preview_idle(TimePoint when);

  LoopApi& loop_;
  ConfigService& config_;
  LoopBridge bridge_;
  EventHub hub_;
  LoopBackend backend_;
  WebApi api_;
  WebServer server_;
  Duration preview_idle_ = kPreviewIdle;  // loop thread, like the three below
  TimePoint last_preview_{};
  TimerId idle_timer_ = 0;
  bool idle_armed_ = false;
};

}  // namespace ld
