#include "device/simulator.h"
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include "core/log.h"

namespace ld {

SimulatorSink::SimulatorSink(std::filesystem::path dir) : dir_(std::move(dir)) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
}

static void write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& data) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void SimulatorSink::key_image(int slot, const Image& image) {
  write_file(dir_ / std::format("key{}.jpg", slot), jpeg_.encode(image, 85, Orientation::None));
}

void SimulatorSink::strip_image(const Image& image) { write_file(dir_ / "strip.jpg", jpeg_.encode(image, 80, Orientation::None)); }

void SimulatorSink::rings(std::span<const Rgb> colors, std::uint8_t brightness) {
  std::ofstream f(dir_ / "rings.txt", std::ios::trunc);
  for (Rgb c : colors) f << format_color(c) << ' ';
  f << static_cast<int>(brightness) << '\n';
}

std::vector<SimStep> parse_sim_script(std::string_view text, const DeviceLayout& layout, std::string* error) {
  std::vector<SimStep> steps;
  std::istringstream in{std::string(text)};
  std::string line;
  int line_no = 0;
  auto fail = [&](const std::string& why) {
    if (error) *error = std::format("line {}: {}", line_no, why);
    return std::vector<SimStep>{};
  };
  if (error) error->clear();
  const int keys = layout.key_count(), knobs = layout.knobs;
  while (std::getline(in, line)) {
    ++line_no;
    if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    SimStep s;
    int a = 0, b = 0;
    if (cmd == "keydown" || cmd == "keyup") {
      if (!(ls >> a) || a < 0 || a >= keys) return fail(std::format("key slot 0-{} expected", keys - 1));
      s.input = {cmd == "keydown" ? InputKind::KeyDown : InputKind::KeyUp, a};
    } else if (cmd == "turn" || cmd == "press") {
      if (knobs == 0) return fail("this device has no knobs");
      if (cmd == "turn") {
        if (!(ls >> a >> b) || a < 0 || a >= knobs || (b != 1 && b != -1))
          return fail(std::format("turn <0-{}> <+1|-1> expected", knobs - 1));
        s.input = {InputKind::KnobTurn, a, b};
      } else {
        if (!(ls >> a) || a < 0 || a >= knobs) return fail(std::format("knob 0-{} expected", knobs - 1));
        s.input = {InputKind::KnobPress, a};
      }
    } else if (cmd == "touchdown" || cmd == "touchmove" || cmd == "touchup") {
      if (!layout.strip) return fail("this device has no touch strip");
      if (cmd == "touchdown") {
        if (!(ls >> a)) return fail("x expected");
        s.input = {InputKind::TouchDown, 0, 0, a};
      } else if (cmd == "touchmove") {
        if (!(ls >> a >> b)) return fail("x y expected");
        s.input = {InputKind::TouchMove, 0, 0, a, b};
      } else {
        s.input = {InputKind::TouchUp};
      }
    } else if (cmd == "wait") {
      if (!(ls >> a) || a < 0) return fail("milliseconds expected");
      s.wait = Duration(a);
    } else {
      return fail("unknown command " + cmd);
    }
    steps.push_back(s);
  }
  return steps;
}

SimInjector::SimInjector(LoopApi& loop, std::filesystem::path file, DeviceLayout layout,
                         std::function<void(const DeviceInput&)> sink)
    : loop_(loop), file_(std::move(file)), layout_(std::move(layout)), sink_(std::move(sink)) {
  poll_timer_ = loop_.every(Duration(250), [this] { poll(); });
}

SimInjector::~SimInjector() {
  loop_.cancel(poll_timer_);
  for (TimerId t : step_timers_) loop_.cancel(t);
}

void SimInjector::poll() {
  std::error_code ec;
  if (!std::filesystem::exists(file_, ec)) return;
  std::stringstream ss;
  {
    std::ifstream f(file_);
    ss << f.rdbuf();
  }
  std::filesystem::remove(file_, ec);
  std::string err;
  auto steps = parse_sim_script(ss.str(), layout_, &err);
  if (!err.empty()) {
    log_warn("simulator script rejected: {}", err);
    return;
  }
  run(std::move(steps), 0);
}

void SimInjector::run(std::vector<SimStep> steps, std::size_t i) {
  for (; i < steps.size(); ++i) {
    if (steps[i].wait.count() > 0) {
      auto rest = std::make_shared<std::vector<SimStep>>(std::move(steps));
      std::size_t next = i + 1;
      step_timers_.push_back(loop_.at(loop_.now() + (*rest)[i].wait, [this, rest, next] { run(std::move(*rest), next); }));
      return;
    }
    sink_(steps[i].input);
  }
}

}  // namespace ld
