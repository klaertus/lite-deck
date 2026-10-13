// src/modules/now_playing/now_playing.cpp
#include "modules/now_playing/now_playing.h"
#include "modules/common/fit.h"
#include "modules/common/marquee.h"
#include <algorithm>
#include <cmath>
#include <chrono>

namespace ld {

std::wstring track_text(const Track& t) { return t.artist.empty() ? t.title : t.title + L" - " + t.artist; }

namespace {

constexpr Rgb kBg{0, 0, 0};
constexpr Rgb kGrey{165, 165, 165};
constexpr Rgb kPausedTitle{187, 187, 187};
constexpr Rgb kGreen{76, 217, 100};
constexpr Rgb kBarTrack{45, 45, 45};
constexpr Rgb kIdleGrey{140, 140, 140};  // shared with the notifications zone's empty text
constexpr Duration kBarTick{1000};

// Declared sizes are the ones rendered at 112 px high; the module passes h * k, so nothing moves by
// default. Rows and alignment do not apply to the strip.
constexpr ElementSpec kStateIcon{.key = "icon", .label = "Play / pause icon", .size = 21.28f, .weight = 400,
                                 .min_size = 8, .max_size = 48, .icon = true, .kind = ElementKind::Icon,
                                 .color = kGreen};
constexpr ElementSpec kTitle{.key = "title", .label = "Title", .size = 32.5f, .weight = 600, .min_size = 10,
                             .max_size = 60};
constexpr ElementSpec kArtist{.key = "artist", .label = "Artist", .size = 26, .weight = 400, .min_size = 8,
                              .max_size = 48, .color = kGrey};
constexpr ElementSpec kIdle{.key = "idle", .label = "\"Nothing playing\"", .size = 22.5f, .weight = 600,
                            .min_size = 8, .max_size = 60, .color = kIdleGrey};  // same style as "No notifications"
constexpr ElementSpec kBar{.key = "bar", .label = "Progress bar", .size = 8, .weight = 400, .min_size = 8,
                           .max_size = 8, .kind = ElementKind::Graph, .color = kGreen};

// Play triangle or pause bars, `size` px tall, centred on `cy`, in `color`.
void draw_state_icon(Canvas& c, bool playing, float x, float cy, float size, Rgb color) {
  float top = cy - size / 2;
  if (playing) {
    c.fill_triangle(PointF{x, top}, PointF{x, top + size}, PointF{x + size * 0.87f, cy}, color);
  } else {
    float bar = size * 0.3f;
    c.fill_rect(RectF{x, top, bar, size}, color);
    c.fill_rect(RectF{x + size * 0.55f, top, bar, size}, color);
  }
}

class NowPlayingModule final : public Module {
public:
  explicit NowPlayingModule(std::function<std::unique_ptr<NowPlayingSource>()> f) : factory_(std::move(f)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    ctx_ = &ctx;
    speed_ = s.at("scroll_speed").get<double>();
    color_ = parse_color(s.at("color").get<std::string>()).value_or(Rgb{255, 255, 255});
    source_ = factory_();
    ctx.every(kBarTick, [this] {
      if (bar_visible() && track_->playing && bar_fill(bar_width_) != bar_px_) ctx_->invalidate();
    });
    Poster p = ctx.poster();
    source_->start([p, this](std::optional<Track> t) {
      p.post([this, t = std::move(t)] { on_track(t); });
    });
  }
  // Layout: state icon on the left, title (bold) above the artist (grey). Scrolls only while playing.
  // Each element goes through the slot's settings: shown, size, weight,
  // colour.
  void render(Canvas& c) override {
    c.clear(kBg);
    const float h = c.height();
    if (!has_text()) {
      overflow_ = false;
      const ResolvedStyle rs = c.resolve(kIdle, ElementInput{.state = ElementState::Idle, .size = h * 0.2f});
      if (!rs.visible) return;
      TextStyle st = c.style(kIdle, rs);
      st.align = HAlign::Center;  // centred in the zone, both ways
      st.valign = VAlign::Center;
      fit_text(c, L"Nothing playing", RectF{12, 0, c.width() - 24, h}, st, kIdle.min_size);
      return;
    }
    const bool playing = track_->playing;
    const ResolvedStyle icon = c.resolve(kStateIcon, ElementInput{.color = playing ? kGreen : kGrey, .size = h * 0.19f});
    if (icon.visible) draw_state_icon(c, playing, 18, h / 2, icon.size, icon.color);
    const float x = 18 + icon.size + 16;
    const float w = c.width() - x - 14;
    const ResolvedStyle title_rs = c.resolve(kTitle, ElementInput{.color = playing ? color_ : kPausedTitle, .size = h * 0.29f});
    TextStyle title = c.style(kTitle, title_rs);
    title.align = HAlign::Left;
    const ResolvedStyle artist_rs = c.resolve(kArtist, ElementInput{.color = kGrey, .size = title.size * 0.8f});
    TextStyle artist = c.style(kArtist, artist_rs);  // follows the title unless set
    artist.align = HAlign::Left;
    const double off = playing ? scroll_offset(std::chrono::duration_cast<Duration>(ctx_->now() - since_), speed_) : 0.0;
    const MarqueeOptions opt{kBg};
    const std::wstring& a = track_->artist;
    const bool bar = bar_visible();
    const float ty = a.empty() ? (bar ? h * 0.45f : h / 2) : (bar ? h * 0.33f : h * 0.36f);
    const float ay = bar ? h * 0.66f : h * 0.71f;
    bool moving = false, fast = false;
    if (title_rs.visible)
    {
      const bool m = draw_marquee(c, track_->title, RectF{x, ty - title.size * 0.75f, w, title.size * 1.5f}, title, off, opt);
      moving |= m;
      if (m) fast |= !scroll_is_slow(off, c.measure(track_->title, title), marquee_gap(title));
    }
    if (!a.empty() && artist_rs.visible)
    {
      const bool m = draw_marquee(c, a, RectF{x, ay - artist.size * 0.75f, w, artist.size * 1.5f}, artist, off, opt);
      moving |= m;
      if (m) fast |= !scroll_is_slow(off, c.measure(a, artist), marquee_gap(artist));
    }
    overflow_ = playing && moving;
    slow_ = overflow_ && !fast;
    if (bar) {
      const float by = h * 0.88f, bh = 3;
      bar_width_ = w;
      bar_px_ = bar_fill(w);
      const ResolvedStyle bar_rs = c.resolve(kBar, ElementInput{.color = playing ? kGreen : kGrey});
      if (bar_rs.visible) {
        c.fill_round_rect(RectF{x, by, w, bh}, bh / 2, kBarTrack);
        if (bar_px_ > 0) c.fill_round_rect(RectF{x, by, static_cast<float>(bar_px_), bh}, bh / 2, bar_rs.color);
      }
    }
  }
  bool animating() const override { return overflow_ && track_.has_value(); }
  bool slow_animation() const override { return slow_ && animating(); }

private:
  // No string built: called from the 1 s tick and twice per frame.
  bool has_text() const { return track_ && !(track_->title.empty() && track_->artist.empty()); }
  bool bar_visible() const { return has_text() && track_->duration_s > 0; }
  // Filled width in whole pixels for a bar `width` px wide; the position advances while playing.
  int bar_fill(float width) const {
    double pos = track_->position_s;
    if (track_->playing) pos += std::chrono::duration<double>(ctx_->now() - received_).count();
    double frac = std::clamp(pos / track_->duration_s, 0.0, 1.0);
    return static_cast<int>(frac * width);
  }
  // Same report as the current track: equal text, state and duration, and a position within 0.5 s of
  // where the current one has advanced to (players send several events per change).
  bool is_duplicate(const std::optional<Track>& t) const {
    if (!t || !track_) return !t && !track_;
    if (t->title != track_->title || t->artist != track_->artist || t->playing != track_->playing ||
        t->duration_s != track_->duration_s)
      return false;
    double now_pos = track_->position_s;
    if (track_->playing) now_pos += std::chrono::duration<double>(ctx_->now() - received_).count();
    return std::abs(t->position_s - now_pos) <= 0.5;
  }
  void on_track(const std::optional<Track>& t) {
    if (is_duplicate(t)) return;
    const bool same_text = t && track_ && t->title == track_->title && t->artist == track_->artist;
    const bool resumed = t && track_ && t->playing && !track_->playing;
    track_ = t;
    received_ = ctx_->now();
    if (!same_text || resumed) since_ = ctx_->now();  // scroll restarts from the beginning
    if (!track_) overflow_ = slow_ = false;
    ctx_->invalidate();
  }
  std::function<std::unique_ptr<NowPlayingSource>()> factory_;
  std::unique_ptr<NowPlayingSource> source_;
  ModuleContext* ctx_ = nullptr;
  std::optional<Track> track_;
  TimePoint since_{};
  TimePoint received_{};  // when track_ (and its position) was reported
  float bar_width_ = 0;
  int bar_px_ = -1;
  double speed_ = 2;
  Rgb color_{255, 255, 255};
  bool overflow_ = false;
  bool slow_ = false;  // every scrolling text has passed kFastLoops times
};

}  // namespace

std::unique_ptr<Module> make_now_playing_module(std::function<std::unique_ptr<NowPlayingSource>()> factory) {
  return std::make_unique<NowPlayingModule>(std::move(factory));
}

ModuleInfo now_playing_module_info() {
  ModuleInfo m;
  m.id = "now_playing";
  m.name = "Now playing";
  m.description = "Song title and artist, play state and progress.";
  m.category = "Media & sound";
  m.hue = "#4cd964";
  m.slots = {SlotKind::StripZone};
  m.settings_schema = nlohmann::json::parse(R"json({"fields":[
    {"key":"scroll_speed","type":"number","default":2,"min":0.5,"max":10,"label":"Scroll speed"},
    {"key":"color","type":"color","default":"#ffffff","label":"Color"}]})json");
  m.elements = {kStateIcon, kTitle, kArtist, kIdle, kBar};
  m.create = [] { return make_now_playing_module(make_gsmtc_source); };
  return m;
}

}  // namespace ld
