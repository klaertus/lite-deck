#pragma once
#include <functional>
#include "modules/audio/audio_endpoint.h"
#include "modules/module.h"

namespace ld {
using AudioProviderFactory = std::function<std::unique_ptr<AudioEndpointProvider>()>;
struct AudioFactory {
  AudioProviderFactory provider;
  Rgb muted_ring;  // defaults when a setting is missing (tests); the schemas give the real ones
  // Volume only: sends a media key (VK_VOLUME_UP/DOWN/MUTE) so Windows shows its own volume overlay.
  std::function<void(unsigned short vk)> media_key = {};
  Rgb normal_ring{0x00, 0x28, 0x3e};
};
float step_volume(float current, int delta, int step_percent);
std::unique_ptr<Module> make_audio_knob(AudioFactory factory);
ModuleInfo volume_module_info();
ModuleInfo mic_module_info();
}  // namespace ld
