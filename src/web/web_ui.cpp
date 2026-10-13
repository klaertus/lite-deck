// src/web/web_ui.cpp
#include "web/web_ui.h"
#include <algorithm>
#include "core/log.h"
#include "core/strings.h"
#include "render/fonts.h"
#include "web/auth.h"

namespace ld {

LoopBackend::LoopBackend(LoopBridge& bridge, ConfigService& config, Deck& deck, std::function<std::string()> device_state)
    : bridge_(bridge), config_(config), deck_(deck), device_state_(std::move(device_state)),
      targets_(static_cast<std::size_t>(deck.layout().key_count()) + 1),
      gens_(std::make_unique<std::atomic<std::uint64_t>[]>(targets_)) {}

std::size_t LoopBackend::slot(int target) const {
  return target < 0 ? targets_ - 1 : std::min(static_cast<std::size_t>(target), targets_ - 1);
}

std::optional<ConfigSnapshot> LoopBackend::config() {
  return bridge_.call([this] { return ConfigSnapshot{config_.document(), config_.revision()}; });
}

std::optional<ConfigUpdate> LoopBackend::update_config(nlohmann::json doc, std::uint64_t expected_revision) {
  return bridge_.call([this, doc = std::move(doc), expected_revision] {
    UpdateResult r = config_.update(doc, expected_revision);
    return ConfigUpdate{std::move(r), ConfigSnapshot{config_.document(), config_.revision()}};
  });
}

std::optional<std::string> LoopBackend::preview(int target) {
  return bridge_.call([this, target] {
    out_ = true;
    if (on_preview) on_preview();
    const std::size_t i = slot(target);
    const std::uint64_t gen = gens_[i].load();
    if (cache_.size() != targets_) cache_.resize(targets_);
    Cached& c = cache_[i];
    // The one copy of a body: the server moves it from here on.
    if (!c.jpeg.empty() && c.gen == gen) return std::string(c.jpeg.begin(), c.jpeg.end());  // every page, one encode
    // Encoded straight into the entry's kept buffer: a steady stream of new frames moves no large block.
    c.gen = gen;
    try {
      if (!jpeg_) jpeg_.emplace();
      const bool drawn = deck_.preview(target, [&](const Image& image) {
        jpeg_->encode(image, target < 0 ? kStripQuality : kKeyQuality, c.jpeg);
        ++encodes_;
      });
      if (!drawn) c.jpeg.clear();
    } catch (const std::exception& e) {
      log_error("preview {} failed: {}", target, e.what());
      c.jpeg.clear();
    }
    std::string out(c.jpeg.begin(), c.jpeg.end());
    if (preview_stats().bytes > kMaxPreviewCache) std::vector<std::uint8_t>().swap(c.jpeg);  // served, not kept
    return out;
  });
}

std::string LoopBackend::preview_tag(int target) {
  return "\"" + std::to_string(epoch_.load()) + "-" + std::to_string(gens_[slot(target)].load()) + "\"";
}

void LoopBackend::frame(int target) { gens_[slot(target)].fetch_add(1); }

void LoopBackend::drop_previews() {
  std::vector<Cached>().swap(cache_);
  jpeg_.reset();
  out_ = false;
  epoch_.fetch_add(1);
}

LoopBackend::PreviewStats LoopBackend::preview_stats() const {
  PreviewStats s{encodes_, 0, jpeg_.has_value()};
  for (const Cached& c : cache_) s.bytes += c.jpeg.capacity();
  return s;
}

std::vector<std::string> LoopBackend::fonts() { return system_font_families(); }

std::optional<nlohmann::json> LoopBackend::status() {
  return bridge_.call([this] {
    return nlohmann::json{{"device", device_state_ ? device_state_() : std::string("unknown")},
                          {"revision", config_.revision()},
                          {"config_errors", errors_to_json(config_.last_errors())}};
  });
}

WebUi::WebUi(LoopApi& loop, ConfigService& config, Deck& deck, std::function<std::string()> device_state,
             AssetLookup assets)
    : loop_(loop), config_(config), bridge_(loop), backend_(bridge_, config, deck, std::move(device_state)),
      api_(random_token(), backend_, module_registry(), std::move(assets), deck.layout()),
      server_(api_, hub_) {
  // The last page closed: free the previews on the loop (a web thread runs this; at shutdown the
  // bridge is already closed and it returns at once).
  hub_.on_idle([this] { bridge_.call([this] { backend_.drop_previews(); return 0; }); });
  backend_.on_preview = [this] { preview_served(); };
}

// No page open and no preview asked for a while (a script that stopped polling): free them too.
void WebUi::preview_served() {
  last_preview_ = loop_.now();
  if (!idle_armed_) arm_preview_idle(last_preview_ + preview_idle_);
}

void WebUi::arm_preview_idle(TimePoint when) {
  idle_armed_ = true;
  idle_timer_ = loop_.at(when, [this] {
    idle_armed_ = false;
    if (!backend_.previews_out() || hub_.streams() > 0) return;  // a page open: its closing frees them
    const TimePoint due = last_preview_ + preview_idle_;
    if (loop_.now() >= due) backend_.drop_previews();
    else arm_preview_idle(due);
  });
}

WebUi::~WebUi() { stop(); }

bool WebUi::start(int port) { return server_.start(port); }

void WebUi::stop() {
  if (idle_armed_) {
    loop_.cancel(idle_timer_);
    idle_armed_ = false;
  }
  bridge_.close();  // first: a request waiting on the loop gives up at once
  server_.stop();   // then the event streams, the listener and the pool
}

bool WebUi::running() const { return server_.running(); }

std::wstring WebUi::config_url() const {
  return running() ? widen(config_page_url(server_.port(), api_.token())) : std::wstring();
}

void WebUi::config_changed() {
  if (hub_.streams() == 0) return;  // no page open: build nothing
  hub_.publish("config", nlohmann::json{{"revision", config_.revision()}, {"errors", errors_to_json(config_.last_errors())}});
}

void WebUi::device_changed(const std::string& state) {
  if (hub_.streams() == 0) return;
  hub_.publish("device", nlohmann::json{{"state", state}});
}

void WebUi::frame(int target) {
  const bool pages = hub_.streams() > 0;
  if (!pages && !backend_.previews_out()) return;  // no page open, no preview out: no work at all
  backend_.frame(target);  // the generation first: a page told of the frame gets it
  if (pages) hub_.frame(target);
}

}  // namespace ld
