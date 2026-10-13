#include "modules/media/media.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include "core/log.h"
#include "modules/common/ring.h"
#include "modules/common/state_settings.h"

namespace ld {

void send_media_key(unsigned short vk) {
  INPUT in[2]{};
  in[0].type = in[1].type = INPUT_KEYBOARD;
  in[0].ki.wVk = in[1].ki.wVk = vk;
  in[1].ki.dwFlags = KEYEVENTF_KEYUP;
  // SendInput returns fewer events than sent when input is blocked, typically by UIPI while an elevated
  // window has the focus (Windows does not always report that case). Logged once.
  static std::atomic<bool> warned{false};
  if (SendInput(2, in, sizeof(INPUT)) != 2 && !warned.exchange(true))
    log_warn("media keys: SendInput blocked (error {}); an elevated window may have the focus", GetLastError());
}

namespace {

constexpr RingStateSpec kNormal{"normal", "Normal", "color", "brightness"};
constexpr Rgb kMediaNormal{0x0c, 0x3a, 0x14};

class MediaModule final : public Module {
public:
  explicit MediaModule(KeySender s) : send_(s ? std::move(s) : KeySender(send_media_key)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    ctx.set_ring_color(ring_color(s, kNormal, kMediaNormal, 100));
  }
  void on_event(const InputEvent& e) override {
    if (e.kind == EventKind::Rotate) {  // one track per notch
      for (int i = 0; i < std::abs(e.delta); ++i) send_(e.delta > 0 ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PREV_TRACK);
    }
    else if (e.kind == EventKind::Press) send_(VK_MEDIA_PLAY_PAUSE);
  }
  void render(Canvas&) override {}

private:
  KeySender send_;
};

}  // namespace

std::unique_ptr<Module> make_media_module(KeySender sender) { return std::make_unique<MediaModule>(std::move(sender)); }

ModuleInfo media_module_info() {
  ModuleInfo m;
  m.id = "media";
  m.name = "Media controls";
  m.description = "Turn: next or previous track. Press: play or pause.";
  m.category = "Media & sound";
  m.hue = "#4cd964";
  m.slots = {SlotKind::Knob};
  m.settings_schema = with_fields(nlohmann::json::parse(R"({"fields":[]})"), ring_fields(kNormal, kMediaNormal, 100));
  m.ring_states = {kNormal};
  m.create = [] { return make_media_module({}); };
  return m;
}

}  // namespace ld
