// src/layout/deck.h
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "core/config.h"
#include "core/http.h"
#include "core/loop_api.h"
#include "core/state_store.h"
#include "device/device.h"
#include "layout/deck_sink.h"
#include "layout/gestures.h"
#include "modules/module.h"
#include "render/renderer.h"

namespace ld {

struct DeckServices {
  HttpClient* http = nullptr;
  StateStore* state = nullptr;
};

// Owns the module instances placed on keys, knobs and strip zones; routes input to them,
// renders dirty slots and pushes images and ring colors to the sink.
class Deck {
public:
  static constexpr Duration kLongPress{500};
  static constexpr Duration kFrame{33};
  static constexpr Duration kSlowFrame{50};  // 20 fps when only slow animations run

  Deck(LoopApi& loop, Renderer& renderer, DeckSink& sink, const DeviceLayout& layout, DeckServices services = {});
  ~Deck();
  Deck(const Deck&) = delete;
  Deck& operator=(const Deck&) = delete;

  void apply(const Config& config, const ModuleLookup& lookup = find_module);
  void on_input(const DeviceInput& in);
  void redraw_all();
  void suspend();
  void resume();
  bool suspended() const { return suspended_; }
  const DeviceLayout& layout() const { return layout_; }

  // Loop thread only. Lends the last frame of key `target` (0 .. key_count - 1), or of the whole strip
  // (-1), to `use` and returns true; false (`use` not called) when there is no such target.
  bool preview(int target, const std::function<void(const Image&)>& use);
  // Called on the loop thread after a key or the strip (-1) got a new frame.
  std::function<void(int target)> on_frame;

private:
  struct Slot;
  class SlotContext;

  std::unique_ptr<Slot> make_slot(SlotKind kind, int index, RectF rect, const std::optional<SlotConfig>& sc);
  struct Want {
    SlotKind kind;
    int index;
    RectF rect;
    std::optional<SlotConfig> cfg;
  };
  static std::string state_key(const std::string& module, SlotKind kind, int index);
  std::vector<Want> wanted() const;
  void build() { reconcile(true); }
  void reconcile(bool full);
  void reset_input();
  void clear_slots();
  void dispatch(Slot& s, const InputEvent& e);
  void on_gesture(const std::optional<Gesture>& g);
  void mark_dirty(Slot& s);
  void mark_broken(Slot& s, const char* where, const char* what);
  void request_render();
  void render_pass();
  void render_slot(Slot& s, Canvas& c);
  // The slot's look: fonts, element overrides, palette and, on a key only, the text roles.
  SlotLook look_for(const SlotConfig& sc, SlotKind kind);
  std::wstring installed_font(const std::string& family);
  std::wstring role_font(const std::string& family);
  void note_missing_font(const std::string& family, std::string_view instead);
  void update_animation();
  void update_rings(bool force);

  LoopApi& loop_;
  Renderer& renderer_;
  DeckSink& sink_;
  DeviceLayout layout_;  // the active device's data sheet (copied)
  DeckServices services_;
  Config config_;
  ModuleLookup lookup_ = find_module;
  std::vector<std::unique_ptr<Slot>> keys_, knobs_, zones_;
  std::vector<std::unique_ptr<Surface>> key_surfaces_;
  std::unique_ptr<Surface> strip_surface_;
  bool render_posted_ = false;
  bool strip_force_ = false;
  bool suspended_ = false;
  bool building_ = false;
  bool applying_ = false;
  int last_ring_level_ = -1;
  TimerId anim_timer_ = 0;
  Duration anim_period_{0};
  std::vector<TimerId> long_timers_;  // one per key of the layout
  std::vector<bool> long_fired_;
  GestureRecognizer gestures_;
  std::set<std::string> missing_fonts_;  // already logged once
  std::vector<Rgb> ring_scratch_, last_rings_;  // one per knob; reused, no allocation once sized
  bool rings_sent_ = false;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace ld
