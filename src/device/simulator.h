#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "core/loop_api.h"
#include "device/device.h"
#include "layout/deck_sink.h"
#include "render/jpeg.h"

namespace ld {

// Writes each frame as a JPEG file in `dir` (unrotated: keys at 85, the strip at 80, as the Deck did
// for the simulator) and the ring colours to rings.txt.
class SimulatorSink final : public DeckSink {
public:
  explicit SimulatorSink(std::filesystem::path dir);
  void key_image(int slot, const Image& image) override;
  void strip_image(const Image& image) override;
  void rings(std::span<const Rgb> colors, std::uint8_t brightness) override;

private:
  std::filesystem::path dir_;
  JpegEncoder jpeg_;
};

struct SimStep {
  DeviceInput input;  // kind None for pure waits
  Duration wait{0};
};

// The script of an inject.txt file; keys, knobs and touch commands follow `layout` (a device without a
// strip refuses touch commands). On an error: nothing, and `error` names the line.
std::vector<SimStep> parse_sim_script(std::string_view text, const DeviceLayout& layout, std::string* error);

// Polls `file` every 250 ms; when it appears, runs its script on the loop and deletes it.
class SimInjector {
public:
  SimInjector(LoopApi& loop, std::filesystem::path file, DeviceLayout layout, std::function<void(const DeviceInput&)> sink);
  ~SimInjector();

private:
  void poll();
  void run(std::vector<SimStep> steps, std::size_t i);
  LoopApi& loop_;
  std::filesystem::path file_;
  DeviceLayout layout_;
  std::function<void(const DeviceInput&)> sink_;
  TimerId poll_timer_ = 0;
  std::vector<TimerId> step_timers_;
};

}  // namespace ld
