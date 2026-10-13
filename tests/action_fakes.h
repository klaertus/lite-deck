// tests/action_fakes.h: fakes for the action keys (launcher, hotkey). Nothing here launches a
// process, sends a keystroke or touches a window.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "modules/hotkey/hotkey.h"
#include "modules/launcher/launcher.h"

namespace fakes {

// Runs a job at once, on the calling thread (the posted result still waits for a drain).
inline ld::Runner inline_runner() {
  return [](std::function<void()> job) { job(); };
}

// Keeps the jobs until run_all(): a job "in flight".
struct HeldRunner {
  std::shared_ptr<std::vector<std::function<void()>>> jobs = std::make_shared<std::vector<std::function<void()>>>();
  ld::Runner runner() {
    return [jobs = jobs](std::function<void()> job) { jobs->push_back(std::move(job)); };
  }
  void run_all() {
    auto now = std::move(*jobs);
    jobs->clear();
    for (auto& j : now) j();
  }
};

// A disc of `color` on transparent, w x h, premultiplied BGRA.
inline std::shared_ptr<ld::Bitmap> disc(int w, int h, ld::Rgb color) {
  auto b = std::make_shared<ld::Bitmap>();
  b->width = w;
  b->height = h;
  b->bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
  const float cx = w / 2.f, cy = h / 2.f, r = std::min(w, h) / 2.f;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
      const float a = std::clamp(r - d, 0.f, 1.f);
      std::uint8_t* p = &b->bgra[(static_cast<std::size_t>(y) * w + x) * 4];
      p[0] = static_cast<std::uint8_t>(color.b * a);
      p[1] = static_cast<std::uint8_t>(color.g * a);
      p[2] = static_cast<std::uint8_t>(color.r * a);
      p[3] = static_cast<std::uint8_t>(255 * a);
    }
  return b;
}

struct IconLoader final : ld::IconLoader {
  std::map<std::wstring, std::shared_ptr<const ld::Bitmap>> icons;  // by path; others: not found
  std::vector<ld::IconRequest> asked;
  std::shared_ptr<const ld::Bitmap> load(const ld::IconRequest& r) override {
    asked.push_back(r);
    auto it = icons.find(r.path);
    return it == icons.end() ? nullptr : it->second;
  }
};

struct Launcher final : ld::Launcher {
  std::vector<ld::LaunchRequest> launched;
  std::vector<std::wstring> focus_asked;
  std::optional<std::string> error;  // what launch() answers
  bool running = false;              // what focus_existing() answers
  bool throws = false;               // launch() throws (a bug, an allocation failure)
  std::optional<std::string> launch(const ld::LaunchRequest& r) override {
    if (throws) throw std::runtime_error("boom");
    launched.push_back(r);
    return error;
  }
  bool focus_existing(const std::wstring& exe) override {
    focus_asked.push_back(exe);
    return running;
  }
};

// A US layout for a few characters; scan codes are vk + 0x100 (only their place is checked).
struct Keyboard final : ld::Keyboard {
  std::vector<std::vector<INPUT>> sent;  // one entry per send()
  std::vector<int> pauses;
  std::optional<unsigned> accept;  // set: each SendInput takes at most this many events (blocked)
  int begins = 0;
  void begin() override { ++begins; }
  unsigned send(std::span<const INPUT> in) override {
    sent.emplace_back(in.begin(), in.end());
    return accept ? std::min<unsigned>(*accept, static_cast<unsigned>(in.size())) : static_cast<unsigned>(in.size());
  }
  std::optional<CharKey> key_for_char(wchar_t ch) override {
    switch (ch) {
      case L'-': return CharKey{.vk = VK_OEM_MINUS};
      case L'/': return CharKey{.vk = VK_OEM_2};
      case L'?': return CharKey{.vk = VK_OEM_2, .shift = true};
      case L'@': return CharKey{.vk = '2', .shift = true};
      case L'€': return CharKey{.vk = 'E', .ctrl = true, .alt = true};  // AltGr+E
      default: return std::nullopt;
    }
  }
  std::uint16_t scan_code(std::uint16_t vk) override { return static_cast<std::uint16_t>(vk + 0x100); }
  void pause(int ms) override { pauses.push_back(ms); }
  std::vector<INPUT> all() const {
    std::vector<INPUT> out;
    for (const auto& s : sent) out.insert(out.end(), s.begin(), s.end());
    return out;
  }
};

// Top-level windows in z-order; records what was activated.
struct Windows final : ld::WindowSystem {
  std::vector<ld::WindowInfo> list;
  std::vector<std::pair<std::uintptr_t, bool>> activated;  // id, restored
  std::vector<ld::WindowInfo> windows() override { return list; }
  bool activate(std::uintptr_t id, bool restore) override {
    activated.emplace_back(id, restore);
    return true;
  }
};

}  // namespace fakes
