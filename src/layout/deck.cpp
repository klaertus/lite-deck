// src/layout/deck.cpp
#include "layout/deck.h"
#include <algorithm>
#include <set>
#include "core/log.h"
#include "core/strings.h"
#include "render/fonts.h"
#include "render/key_renderer.h"

namespace ld {

struct Deck::Slot {
  SlotKind kind = SlotKind::Key;
  int index = 0;
  RectF rect;
  std::string module_id;
  std::optional<SlotConfig> config;  // what the slot was built from: unchanged slots are kept
  SlotLook look;  // resolved from config and the global appearance; what render_slot gives the canvas
  bool dirty = true;
  bool broken = false;
  std::optional<Rgb> ring;
  std::unique_ptr<SlotContext> ctx;  // declared before module: destroyed after it
  std::unique_ptr<Module> module;
  ~Slot();
};

class Deck::SlotContext final : public ModuleContext {
public:
  SlotContext(Deck& d, Slot& s) : d_(d), s_(s) {
    target_->run = [this](const std::function<void()>& fn) { guarded(fn); };
  }
  ~SlotContext() override {
    for (TimerId id : timers_) d_.loop_.cancel(id);
  }
  void detach() {
    detached_ = true;
    target_->alive.store(false);
  }
  TimePoint now() const override { return d_.loop_.now(); }
  TimerId at(TimePoint when, std::function<void()> fn) override {
    if (detached_) return 0;
    auto holder = std::make_shared<TimerId>(0);
    TimerId id = d_.loop_.at(when, [this, holder, fn = std::move(fn)] {
      timers_.erase(*holder);
      guarded(fn);
    });
    *holder = id;
    timers_.insert(id);
    return id;
  }
  TimerId every(Duration period, std::function<void()> fn) override {
    if (detached_) return 0;
    TimerId id = d_.loop_.every(period, [this, fn = std::move(fn)] { guarded(fn); });
    timers_.insert(id);
    return id;
  }
  void cancel(TimerId id) override {
    timers_.erase(id);
    d_.loop_.cancel(id);
  }
  void post(std::function<void()> fn) override { poster().post(std::move(fn)); }
  Poster poster() override { return Poster(d_.loop_, target_); }
  void invalidate() override {
    if (!detached_) d_.mark_dirty(s_);
  }
  void set_ring_color(Rgb color) override {
    if (detached_ || s_.kind != SlotKind::Knob) return;
    s_.ring = color;
    d_.update_rings(false);
  }
  void clear_ring_color() override {
    if (detached_ || s_.kind != SlotKind::Knob || !s_.ring) return;
    s_.ring.reset();
    d_.update_rings(false);
  }
  using ModuleContext::http_get;
  void http_get(const HttpRequest& request, std::function<void(const HttpResponse&)> done) override {
    Poster p = poster();
    if (!d_.services_.http) {
      p.post([done = std::move(done)] { done(HttpResponse{0, "", "http unavailable"}); });
      return;
    }
    // On the isolated lane a slot has at most one request in flight: a second one is refused at once,
    // so a module cannot fill the lane while its host does not answer.
    if (request.lane == HttpLane::Isolated) {
      if (*isolated_busy_) {
        p.post([done = std::move(done)] { done(HttpResponse{0, "", "busy"}); });
        return;
      }
      *isolated_busy_ = true;
      done = [busy = isolated_busy_, done = std::move(done)](const HttpResponse& r) {
        *busy = false;
        done(r);
      };
    }
    d_.services_.http->get(
        request, [p, done = std::move(done)](HttpResponse r) { p.post([done, r = std::move(r)] { done(r); }); },
        [t = target_] { return t->alive.load(); });  // a request of a removed module is never sent
  }
  nlohmann::json load_state() override {
    return d_.services_.state ? d_.services_.state->get(state_key()) : nlohmann::json();
  }
  void save_state(const nlohmann::json& state) override {
    if (d_.services_.state && !detached_) d_.services_.state->set(state_key(), state);
  }

private:
  std::string state_key() const { return Deck::state_key(s_.module_id, s_.kind, s_.index); }
  void guarded(const std::function<void()>& fn) {
    if (detached_ || s_.broken) return;
    try {
      fn();
    } catch (const std::exception& e) {
      d_.mark_broken(s_, "timer", e.what());
    } catch (...) {
      d_.mark_broken(s_, "timer", "unknown");
    }
  }
  Deck& d_;
  Slot& s_;
  bool detached_ = false;
  std::set<TimerId> timers_;
  std::shared_ptr<PostTarget> target_ = std::make_shared<PostTarget>();
  std::shared_ptr<bool> isolated_busy_ = std::make_shared<bool>(false);  // loop thread only
};

Deck::Slot::~Slot() {
  if (ctx) ctx->detach();
}

Deck::Deck(LoopApi& loop, Renderer& renderer, DeckSink& sink, const DeviceLayout& layout, DeckServices services)
    : loop_(loop), renderer_(renderer), sink_(sink), layout_(layout), services_(services),
      long_timers_(static_cast<std::size_t>(layout.key_count()), 0),
      long_fired_(static_cast<std::size_t>(layout.key_count()), false) {
  for (int i = 0; i < layout_.key_count(); ++i)
    key_surfaces_.push_back(std::make_unique<Surface>(renderer_, layout_.key_size.w, layout_.key_size.h));
  if (layout_.strip) strip_surface_ = std::make_unique<Surface>(renderer_, layout_.strip->w, layout_.strip->h);
}

Deck::~Deck() {
  clear_slots();
  alive_.reset();
}

void Deck::apply(const Config& config, const ModuleLookup& lookup) {
  if (applying_) {  // re-entered (e.g. from a module's start()): run after the current apply
    std::weak_ptr<int> alive = alive_;
    loop_.post([this, alive, config, lookup] {
      if (alive.lock()) apply(config, lookup);
    });
    return;
  }
  applying_ = true;
  struct Done {
    bool& flag;
    ~Done() { flag = false; }
  } done{applying_};
  config_ = config;
  lookup_ = lookup;
  if (suspended_) return;  // resume() builds from config_
  reconcile(keys_.empty());
}

std::unique_ptr<Deck::Slot> Deck::make_slot(SlotKind kind, int index, RectF rect, const std::optional<SlotConfig>& sc) {
  auto s = std::make_unique<Slot>();
  s->kind = kind;
  s->index = index;
  s->rect = rect;
  s->config = sc;
  if (!sc) return s;
  s->look = look_for(*sc, kind);
  s->module_id = sc->module;
  const ModuleInfo* info = lookup_(sc->module);
  if (!info || !info->allows(kind)) return s;
  s->ctx = std::make_unique<SlotContext>(*this, *s);
  try {
    s->module = info->create();
    s->module->start(*s->ctx, sc->settings);
  } catch (const std::exception& e) {
    mark_broken(*s, "start", e.what());
  } catch (...) {
    mark_broken(*s, "start", "unknown");
  }
  return s;
}

namespace {
constexpr Rgb kZoneSeparator{51, 51, 51};
}  // namespace

std::string Deck::state_key(const std::string& module, SlotKind kind, int index) {
  const char k = kind == SlotKind::Key ? 'k' : kind == SlotKind::Knob ? 'n' : 'z';
  return module + ":" + k + std::to_string(index);
}

std::vector<Deck::Want> Deck::wanted() const {
  std::vector<Want> w;
  const float kw = static_cast<float>(layout_.key_size.w), kh = static_cast<float>(layout_.key_size.h);
  for (int i = 0; i < layout_.key_count(); ++i) w.push_back({SlotKind::Key, i, RectF{0, 0, kw, kh}, slot_at(config_.keys, i)});
  for (int i = 0; i < layout_.knobs; ++i) w.push_back({SlotKind::Knob, i, RectF{}, slot_at(config_.knobs, i)});
  if (!layout_.strip) return w;  // no strip: the section stays in the config, no zone starts
  const float sh = static_cast<float>(layout_.strip->h);
  float x = 0;
  for (std::size_t i = 0; i < config_.strip.size(); ++i) {
    const auto& z = config_.strip[i];
    w.push_back({SlotKind::StripZone, static_cast<int>(i), RectF{x, 0, static_cast<float>(z.width), sh}, z.slot});
    x += static_cast<float>(z.width);
  }
  return w;
}

void Deck::reset_input() {
  for (auto& t : long_timers_) {
    loop_.cancel(t);
    t = 0;
  }
  std::fill(long_fired_.begin(), long_fired_.end(), false);
  gestures_ = GestureRecognizer{};  // a touch that spans a reload reaches nobody
}

// Builds the slots the config wants, keeping every slot whose module and settings (and width, for
// zones) did not change, even if it moved. `full` = nothing was built yet: draw everything.
void Deck::reconcile(bool full) {
  reset_input();
  building_ = true;
  const std::vector<Want> want = wanted();
  const std::size_t old_zone_count = zones_.size();
  std::vector<std::unique_ptr<Slot>> old;
  for (auto* v : {&keys_, &knobs_, &zones_})
    for (auto& s : *v) old.push_back(std::move(s));
  keys_.clear();
  knobs_.clear();
  zones_.clear();

  auto keeps = [](const Slot& s, const Want& w) {
    if (s.kind != w.kind || s.broken) return false;  // a broken module restarts on every reload
    if (w.kind == SlotKind::StripZone && s.rect.w != w.rect.w) return false;
    if (!w.cfg) return !s.config;  // empty stays empty
    return s.module && s.config && s.config->module == w.cfg->module && s.config->settings == w.cfg->settings;
  };
  std::vector<std::unique_ptr<Slot>> kept(want.size());
  for (std::size_t i = 0; i < want.size(); ++i)  // same place first...
    for (auto& s : old)
      if (s && s->index == want[i].index && keeps(*s, want[i])) {
        kept[i] = std::move(s);
        break;
      }
  for (std::size_t i = 0; i < want.size(); ++i)  // ...then moved
    if (!kept[i])
      for (auto& s : old)
        if (s && keeps(*s, want[i])) {
          kept[i] = std::move(s);
          break;
        }
  old.clear();  // slots nobody wants: their modules stop now, before the new ones start

  if (services_.state) {  // state follows moved modules; keys of vanished slots are purged
    std::map<std::string, std::string> sources;
    std::set<std::string> taken;
    for (std::size_t i = 0; i < want.size(); ++i) {
      if (!kept[i] || !want[i].cfg) continue;
      std::string from = state_key(kept[i]->module_id, kept[i]->kind, kept[i]->index);
      taken.insert(from);
      sources[state_key(want[i].cfg->module, want[i].kind, want[i].index)] = from;
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
      if (kept[i] || !want[i].cfg) continue;
      std::string key = state_key(want[i].cfg->module, want[i].kind, want[i].index);
      if (!sources.contains(key)) sources[key] = taken.contains(key) ? std::string() : key;
    }
    services_.state->rekey(sources);
  }

  bool strip_changed = layout_.strip && config_.strip.size() != old_zone_count;
  for (std::size_t i = 0; i < want.size(); ++i) {
    const Want& w = want[i];
    std::unique_ptr<Slot> s = std::move(kept[i]);
    if (s) {
      if (s->index != w.index || s->rect.x != w.rect.x) {
        s->dirty = true;
        if (w.kind == SlotKind::StripZone) strip_changed = true;
      }
      s->index = w.index;
      s->rect = w.rect;
      s->config = w.cfg;  // same module and settings; the font and element sizes may differ
      if (w.cfg) {
        SlotLook look = look_for(*w.cfg, w.kind);
        if (look != s->look) {  // redrawn, never restarted: the module keeps its state
          s->look = std::move(look);
          s->dirty = true;
        }
      }
    } else {
      s = make_slot(w.kind, w.index, w.rect, w.cfg);
      if (w.kind == SlotKind::StripZone) strip_changed = true;
    }
    (w.kind == SlotKind::Key ? keys_ : w.kind == SlotKind::Knob ? knobs_ : zones_).push_back(std::move(s));
  }
  building_ = false;
  if (full) {
    redraw_all();
    return;
  }
  if (strip_changed) strip_force_ = true;
  request_render();
  update_rings(false);
}

void Deck::clear_slots() {
  loop_.cancel(anim_timer_);
  anim_timer_ = 0;
  reset_input();
  rings_sent_ = false;
  last_ring_level_ = -1;
  auto keys = std::move(keys_);
  auto knobs = std::move(knobs_);
  auto zones = std::move(zones_);
  keys_.clear();
  knobs_.clear();
  zones_.clear();
  keys.clear();
  knobs.clear();
  zones.clear();
}

void Deck::redraw_all() {
  for (auto& s : keys_) s->dirty = true;
  strip_force_ = true;
  request_render();
  update_rings(true);
}

void Deck::suspend() {
  if (suspended_) return;
  suspended_ = true;
  clear_slots();
}

void Deck::resume() {
  if (!suspended_) return;
  suspended_ = false;
  build();
}

void Deck::mark_dirty(Slot& s) {
  if (s.kind == SlotKind::Knob) return;
  s.dirty = true;
  request_render();
}

void Deck::mark_broken(Slot& s, const char* where, const char* what) {
  if (!s.broken) log_error("module '{}' failed in {}: {}", s.module_id, where, what);
  s.broken = true;
  if (s.kind == SlotKind::Knob && s.ring) {
    s.ring.reset();
    update_rings(false);
  }
  mark_dirty(s);
}

void Deck::request_render() {
  if (render_posted_ || suspended_) return;
  render_posted_ = true;
  std::weak_ptr<int> alive = alive_;
  loop_.post([this, alive] {
    if (alive.lock()) render_pass();
  });
}

void Deck::render_slot(Slot& s, Canvas& c) {
  c.set_look(&s.look);
  if (!s.module && !s.broken) return;
  if (!s.broken) {
    try {
      s.module->render(c);
      return;
    } catch (const std::exception& e) {
      log_error("module '{}' failed in render: {}", s.module_id, e.what());
    } catch (...) {
      log_error("module '{}' failed in render", s.module_id);
    }
    s.broken = true;
  }
  c.clear(Rgb{40, 0, 0});
  TextStyle st;
  st.font = kDefaultFont;  // the error mark looks the same whatever the slot's font
  st.size = 48.f * c.height() / kKeyBase;  // 48 at 112 px
  st.bold = true;
  st.color = Rgb{255, 80, 80};
  c.text(L"!", RectF{0, 0, c.width(), c.height()}, st);
}

SlotLook Deck::look_for(const SlotConfig& sc, SlotKind kind) {
  SlotLook look;
  look.font = installed_font(sc.font.empty() ? config_.appearance.font : sc.font);
  look.italic = sc.italic.value_or(config_.appearance.italic);
  look.elements = sc.elements;
  look.states = config_.appearance.states;
  if (kind != SlotKind::Key) return look;  // text roles are for the keys only
  for (const auto& [role, o] : config_.appearance.roles) {
    RoleLook r{o.size, o.weight, {}};
    // Font: the key's own font > the role's > the global one.
    if (sc.font.empty() && !o.font.empty()) r.font = role_font(o.font);
    if (r != RoleLook{}) look.roles[role] = std::move(r);
  }
  return look;
}

// A config may name a font this PC does not have (copied from another PC, font uninstalled): draw
// with Segoe UI and say so once, instead of refusing the config.
std::wstring Deck::installed_font(const std::string& family) {
  std::wstring name = widen(family);
  if (name.empty() || name == kDefaultFont) return kDefaultFont;
  if (font_installed(name)) return name;
  note_missing_font(family, narrow(kDefaultFont));
  return kDefaultFont;
}

// A role's font: "" when this PC does not have it, so the role's elements keep
// the font they would have without it (the key's or the global one); said once.
std::wstring Deck::role_font(const std::string& family) {
  std::wstring name = widen(family);
  if (font_installed(name)) return name;
  note_missing_font(family, "the key's font");
  return {};
}

void Deck::note_missing_font(const std::string& family, std::string_view instead) {
  if (missing_fonts_.size() >= 32 || !missing_fonts_.insert(family).second) return;  // bounded: names come from the config
  std::string shown = family;  // control characters would forge log lines
  for (char& ch : shown)
    if (static_cast<unsigned char>(ch) < 0x20) ch = '?';
  log_warn("font \"{}\" is not installed: {} is used instead", shown, instead);
}

void Deck::render_pass() {
  render_posted_ = false;
  if (suspended_) return;
  for (auto& s : keys_) {
    if (!s->dirty) continue;
    s->dirty = false;
    try {
      Surface& surf = *key_surfaces_[s->index];
      {
        Canvas c = surf.canvas();
        c.clear(Rgb{0, 0, 0});
        render_slot(*s, c);
      }
      {
        SurfacePixels px(surf);  // lent, not copied: the driver encodes it before returning
        sink_.key_image(s->index, px.image());
      }
      if (on_frame) on_frame(s->index);
    } catch (const std::exception& e) {
      log_error("key {} frame dropped: {}", s->index, e.what());
    }
  }
  const bool strip_dirty =
      strip_surface_ && (strip_force_ || std::any_of(zones_.begin(), zones_.end(), [](const auto& z) { return z->dirty; }));
  if (strip_dirty) {
    // Only the dirty zones are drawn again (an animating zone must not re-render its static
    // neighbours 30 times a second); the others keep their pixels in strip_surface_. A forced pass
    // (apply that moved zones, redraw_all, resume) clears the whole strip and draws every zone.
    const bool force = strip_force_;
    strip_force_ = false;
    try {
      if (force) {
        Canvas full = strip_surface_->canvas();
        full.clear(Rgb{0, 0, 0});
      }
      std::vector<bool> drawn(zones_.size(), false);
      for (std::size_t i = 0; i < zones_.size(); ++i) {
        auto& z = zones_[i];
        if (!force && !z->dirty) continue;
        z->dirty = false;
        drawn[i] = true;
        Canvas c = strip_surface_->canvas(z->rect);
        if (!force) c.clear(Rgb{0, 0, 0});  // as after the full clear: modules may not paint every pixel
        render_slot(*z, c);
      }
      if (zones_.size() > 1) {  // thin separator between neighbouring zones, whatever their modules
        // Separator i lies on the first two columns of zone i: only a redrawn zone i lost it.
        Canvas full = strip_surface_->canvas();
        for (std::size_t i = 1; i < zones_.size(); ++i) {
          if (!drawn[i]) continue;
          float x = zones_[i]->rect.x + 1.f;  // 2 px wide, columns x-1..x+1
          const float h = static_cast<float>(strip_surface_->height());
          full.line(PointF{x, h * 14.f / kKeyBase}, PointF{x, h * 98.f / kKeyBase}, kZoneSeparator, 2);  // 14..98 at 112 px
        }
      }
      {
        SurfacePixels px(*strip_surface_);
        sink_.strip_image(px.image());
      }
      if (on_frame) on_frame(-1);
    } catch (const std::exception& e) {
      strip_force_ = true;  // the surface may hold a half-drawn zone: the next strip frame redraws all
      log_error("strip frame dropped: {}", e.what());
    }
  }
  update_animation();
}

bool Deck::preview(int target, const std::function<void(const Image&)>& use) {
  Surface* s = target == -1 ? strip_surface_.get()
               : target >= 0 && target < static_cast<int>(key_surfaces_.size()) ? key_surfaces_[static_cast<std::size_t>(target)].get()
                                                                                : nullptr;
  if (!s) return false;
  SurfacePixels px(*s);
  use(px.image());
  return true;
}

void Deck::update_animation() {
  auto animating = [](const std::unique_ptr<Slot>& s) { return s->module && !s->broken && s->module->animating(); };
  auto fast = [&](const std::unique_ptr<Slot>& s) { return animating(s) && !s->module->slow_animation(); };
  bool any = std::any_of(keys_.begin(), keys_.end(), animating) || std::any_of(zones_.begin(), zones_.end(), animating);
  bool any_fast = std::any_of(keys_.begin(), keys_.end(), fast) || std::any_of(zones_.begin(), zones_.end(), fast);
  // Only slow animations left: 20 fps. Next to a fast one, everything runs at 30 fps (the strip is one image).
  const Duration period = any_fast ? kFrame : kSlowFrame;
  if (anim_timer_ && (!any || period != anim_period_)) {
    loop_.cancel(anim_timer_);
    anim_timer_ = 0;
  }
  if (any && !anim_timer_) {
    anim_period_ = period;
    anim_timer_ = loop_.every(period, [this, animating] {
      for (auto& s : keys_)
        if (animating(s)) s->dirty = true;
      for (auto& z : zones_)
        if (animating(z)) z->dirty = true;
      request_render();
    });
  }
}

void Deck::update_rings(bool force) {
  if (suspended_ || building_ || !layout_.knob_rings) return;
  ring_scratch_.resize(static_cast<std::size_t>(layout_.knobs));
  for (int i = 0; i < layout_.knobs; ++i) ring_scratch_[static_cast<std::size_t>(i)] = ring_color(config_, i);
  for (const auto& k : knobs_)
    if (k->ring) ring_scratch_[static_cast<std::size_t>(k->index)] = *k->ring;
  const int level = config_.ring_brightness;
  if (!force && rings_sent_ && last_rings_ == ring_scratch_ && last_ring_level_ == level) return;
  last_rings_ = ring_scratch_;  // copy-assignment keeps the capacity
  rings_sent_ = true;
  last_ring_level_ = level;
  sink_.rings(ring_scratch_, static_cast<std::uint8_t>(level));
}

void Deck::dispatch(Slot& s, const InputEvent& e) {
  if (!s.module || s.broken) return;
  try {
    s.module->on_event(e);
  } catch (const std::exception& ex) {
    mark_broken(s, "on_event", ex.what());
  } catch (...) {
    mark_broken(s, "on_event", "unknown");
  }
}

void Deck::on_gesture(const std::optional<Gesture>& g) {
  if (!g) return;
  for (auto& z : zones_) {
    if (g->start_x < z->rect.x || g->start_x >= z->rect.x + z->rect.w) continue;
    EventKind kind = g->kind == GestureKind::Tap         ? EventKind::Tap
                     : g->kind == GestureKind::LongTouch ? EventKind::LongPress
                     : g->kind == GestureKind::SwipeLeft ? EventKind::SwipeLeft
                                                         : EventKind::SwipeRight;
    dispatch(*z, InputEvent{kind, 0, g->start_x - static_cast<int>(z->rect.x)});
    return;
  }
}

void Deck::on_input(const DeviceInput& in) {
  if (suspended_) return;
  switch (in.kind) {
    case InputKind::KeyDown: {
      if (in.index < 0 || in.index >= static_cast<int>(keys_.size())) return;
      int i = in.index;
      loop_.cancel(long_timers_[i]);
      long_fired_[i] = false;
      long_timers_[i] = loop_.at(loop_.now() + kLongPress, [this, i] {
        long_timers_[i] = 0;
        long_fired_[i] = true;
        if (i < static_cast<int>(keys_.size())) dispatch(*keys_[i], InputEvent{EventKind::LongPress});
      });
      dispatch(*keys_[i], InputEvent{EventKind::Press});
      break;
    }
    case InputKind::KeyUp: {
      if (in.index < 0 || in.index >= static_cast<int>(keys_.size())) return;
      int i = in.index;
      loop_.cancel(long_timers_[i]);
      long_timers_[i] = 0;
      dispatch(*keys_[i], InputEvent{EventKind::Release});
      if (!long_fired_[i]) dispatch(*keys_[i], InputEvent{EventKind::Tap});
      long_fired_[i] = false;
      break;
    }
    case InputKind::KnobTurn:
      if (in.index >= 0 && in.index < static_cast<int>(knobs_.size()))
        dispatch(*knobs_[in.index], InputEvent{EventKind::Rotate, in.delta});
      break;
    case InputKind::KnobPress:
      if (in.index >= 0 && in.index < static_cast<int>(knobs_.size()))
        dispatch(*knobs_[in.index], InputEvent{EventKind::Press});
      break;
    case InputKind::KnobRelease:
      if (in.index >= 0 && in.index < static_cast<int>(knobs_.size()))
        dispatch(*knobs_[in.index], InputEvent{EventKind::Release});
      break;
    case InputKind::TouchDown:
      if (layout_.strip) on_gesture(gestures_.down(in.x, loop_.now()));
      break;
    case InputKind::TouchMove:
      if (layout_.strip) on_gesture(gestures_.move(in.x, loop_.now()));
      break;
    case InputKind::TouchUp:
      if (layout_.strip) on_gesture(gestures_.up(loop_.now()));
      break;
    case InputKind::None: break;
  }
}

}  // namespace ld
