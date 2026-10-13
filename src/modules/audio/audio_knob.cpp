#include "modules/audio/audio_knob.h"
#include "modules/media/media.h"
#include <algorithm>
#include <cmath>
#include "modules/common/ring.h"
#include "modules/common/state_settings.h"

namespace ld {

float step_volume(float current, int delta, int step) {
  int pct = static_cast<int>(std::lround(current * 100.f / static_cast<float>(step))) * step;  // snap
  pct = std::clamp(pct + delta * step, 0, 100);
  return static_cast<float>(pct) / 100.f;
}

namespace {

constexpr unsigned short kVkVolumeMute = 0xAD;  // VK_VOLUME_MUTE
constexpr unsigned short kVkVolumeDown = 0xAE;  // VK_VOLUME_DOWN
constexpr unsigned short kVkVolumeUp = 0xAF;    // VK_VOLUME_UP

constexpr RingStateSpec kNormal{"normal", "Normal", "color", "brightness"};
constexpr RingStateSpec kMuted{"muted", "Muted", "muted_color", "muted_brightness"};
constexpr Rgb kVolumeNormal{0x00, 0x28, 0x3e}, kVolumeMuted{0x3a, 0x3a, 0x3a};
constexpr Rgb kMicNormal{0x1e, 0x0e, 0x3e}, kMicMuted{0xff, 0x00, 0x00};

class AudioKnob final : public Module {
public:
  explicit AudioKnob(AudioFactory f) : factory_(std::move(f)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    ctx_ = &ctx;
    step_ = s.value("step", 2);
    overlay_ = factory_.media_key && s.value("windows_overlay", true);
    normal_ring_ = ring_color(s, kNormal, factory_.normal_ring, 100);
    muted_ring_ = ring_color(s, kMuted, factory_.muted_ring, 100);
    provider_ = factory_.provider();
    Poster p = ctx.poster();
    provider_->watch([p, this](bool default_changed) {
      p.post([this, default_changed] {
        if (default_changed) provider_->reopen();
        refresh_ring();
      });
    });
    refresh_ring();
  }
  void on_event(const InputEvent& e) override {
    if (overlay_) {  // Windows changes the volume itself (2 % per key) and shows its overlay: no endpoint needed
      if (e.kind == EventKind::Rotate && e.delta != 0) {
        const int keys = std::abs(e.delta) * std::max(1, (step_ + 1) / 2);  // 2 % keys, an odd step rounds up
        for (int i = 0; i < keys; ++i) factory_.media_key(e.delta > 0 ? kVkVolumeUp : kVkVolumeDown);
      } else if (e.kind == EventKind::Press) {
        factory_.media_key(kVkVolumeMute);  // the ring follows through the endpoint notification
      }
      return;
    }
    AudioEndpoint* ep = provider_ ? provider_->endpoint() : nullptr;
    if (!ep) return;
    if (e.kind == EventKind::Rotate) {
      if (auto v = ep->volume()) ep->set_volume(step_volume(*v, e.delta, step_));
    } else if (e.kind == EventKind::Press) {
      if (auto m = ep->muted()) ep->set_muted(!*m);
      refresh_ring();
    }
  }
  void render(Canvas&) override {}

private:
  // The knob always shows its state: normal or muted.
  void refresh_ring() {
    AudioEndpoint* ep = provider_ ? provider_->endpoint() : nullptr;
    auto m = ep ? ep->muted() : std::nullopt;
    ctx_->set_ring_color(m && *m ? muted_ring_ : normal_ring_);
  }
  AudioFactory factory_;
  ModuleContext* ctx_ = nullptr;
  std::unique_ptr<AudioEndpointProvider> provider_;
  int step_ = 2;
  bool overlay_ = false;
  Rgb normal_ring_{}, muted_ring_{};
};

nlohmann::json volume_schema() {
  return nlohmann::json::parse(R"json({"fields":[
    {"key":"step","type":"int","default":2,"min":1,"max":10,"label":"Step (%)"},
    {"key":"windows_overlay","type":"bool","default":true,"label":"Show Windows volume pop-up",
     "hint":"Moves in 2 % steps (an odd step rounds up); not over a window run as admin."}]})json");
}

nlohmann::json step_schema() {
  return nlohmann::json::parse(R"json({"fields":[
    {"key":"step","type":"int","default":2,"min":1,"max":10,"label":"Step (%)"}]})json");
}

// `schema` with the normal and muted ring states.
nlohmann::json with_rings(nlohmann::json schema, Rgb normal, Rgb muted) {
  schema = with_fields(std::move(schema), ring_fields(kNormal, normal, 100));
  return with_fields(std::move(schema), ring_fields(kMuted, muted, 100));
}

}  // namespace

std::unique_ptr<Module> make_audio_knob(AudioFactory f) { return std::make_unique<AudioKnob>(std::move(f)); }

ModuleInfo volume_module_info() {
  ModuleInfo m;
  m.id = "volume";
  m.name = "Volume";
  m.description = "Turn: volume. Press: mute.";
  m.category = "Media & sound";
  m.hue = "#3fb6ff";
  m.slots = {SlotKind::Knob};
  m.settings_schema = with_rings(volume_schema(), kVolumeNormal, kVolumeMuted);
  m.ring_states = {kNormal, kMuted};
  m.create = [] {
    return make_audio_knob(
        {[] { return make_core_audio(AudioFlow::Render); }, kVolumeMuted, send_media_key, kVolumeNormal});
  };
  return m;
}

ModuleInfo mic_module_info() {
  ModuleInfo m;
  m.id = "mic";
  m.name = "Microphone";
  m.description = "Turn: input level. Press: mute.";
  m.category = "Media & sound";
  m.hue = "#9a6bff";
  m.slots = {SlotKind::Knob};
  m.settings_schema = with_rings(step_schema(), kMicNormal, kMicMuted);
  m.ring_states = {kNormal, kMuted};
  m.create = [] {
    return make_audio_knob({[] { return make_core_audio(AudioFlow::Capture); }, kMicMuted, {}, kMicNormal});
  };
  return m;
}

}  // namespace ld
