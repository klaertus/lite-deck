// src/render/key_renderer.cpp
#include "render/key_renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include "modules/common/fit.h"

namespace ld {

RectF KeyTemplate::rect(Row row) const {
  switch (row) {
    case Row::Top: return top;
    case Row::Bottom: return bottom;
    case Row::Line1: return line1;
    case Row::Line2: return line2;
    case Row::Middle: break;
  }
  return middle;
}

KeyTemplate key_template(float width, float height) {
  const float s = height / kKeyBase;
  const float m = 6 * s, w = std::max(0.f, width - 2 * m);  // a key narrower than its margins: empty rows
  auto row = [&](float y, float h) { return RectF{m, y * s, w, h * s}; };
  return KeyTemplate{row(4, 20), row(24, 52), row(76, 32), row(8, 46), row(58, 46), s};
}

std::vector<float> graph_points(const std::deque<float>& values, float lo, float hi) {
  std::vector<float> out;
  out.reserve(values.size());
  if (!(hi > lo)) return out;  // an empty range: nothing to draw, like the plan-4 sparkline
  for (float v : values) out.push_back((v - lo) / (hi - lo));
  return out;
}

namespace {

const ElementSpec* find_spec(std::span<const ElementSpec> specs, std::string_view key) {
  for (const ElementSpec& s : specs)
    if (key == s.key) return &s;
  return nullptr;
}

ElementInput input_of(const KeyElement& e) {
  ElementInput in;
  in.state = e.state;
  in.metric = e.metric;
  in.gradient = e.gradient;
  in.color = e.color;
  in.size = e.size;
  return in;
}

// The width of an image drawn `height` px high, with its proportions.
float image_width(const Bitmap& b, float height) {
  return b.height > 0 ? height * static_cast<float>(b.width) / static_cast<float>(b.height) : 0.f;
}

// Whether an element has something to show: points for a graph, a text, or an icon's image.
bool shows(const ElementSpec& spec, const KeyElement& e) {
  if (spec.kind == ElementKind::Graph) return e.points.size() >= 2;
  if (spec.kind == ElementKind::Icon && e.image) return e.image->width > 0 && e.image->height > 0;
  return !e.text.empty();
}

// An icon's image in its box: `size` px high, aligned like a text, centred vertically.
RectF image_rect(const Bitmap& b, const KeyItem& it) {
  const float h = it.text.size, w = image_width(b, h);
  const float x = it.text.align == HAlign::Left    ? it.rect.x
                  : it.text.align == HAlign::Right ? it.rect.x + it.rect.w - w
                                                   : it.rect.x + (it.rect.w - w) / 2;
  return RectF{x, it.rect.y + (it.rect.h - h) / 2, w, h};
}

// One text of a row while it is laid out.
struct Slot {
  KeyItem* item = nullptr;
  std::wstring_view text;
  float natural = 0;  // width at the resolved size
  float share = 0;    // the most it may take
  float width = 0;    // after the fit, at most `share`
};

// Shares `avail` px: a text narrower than an equal share keeps its width, the others split what is
// left equally (max-min fairness). A short temperature next to a very long label keeps its size.
void share_out(std::vector<Slot*>& slots, float avail) {
  std::vector<Slot*> open = slots;
  float left = avail;
  while (!open.empty()) {
    const float fair = left / static_cast<float>(open.size());
    auto fits = std::stable_partition(open.begin(), open.end(), [fair](const Slot* s) { return s->natural > fair; });
    if (fits == open.end()) {
      for (Slot* s : open) s->share = fair;
      return;
    }
    for (auto it = fits; it != open.end(); ++it) {
      (*it)->share = (*it)->natural;
      left -= (*it)->natural;
    }
    open.erase(fits, open.end());
  }
}

// A minimum size declared at 112 px, at this key's scale (the same int at 112 px).
int scaled_min(int min_size, float scale) {
  return scale == 1.f ? min_size : std::max(1, static_cast<int>(std::lround(static_cast<float>(min_size) * scale)));
}

void place_row(Canvas& c, RectF row, std::array<std::vector<KeyItem*>, 3>& groups, std::span<const KeyElement> content,
               float scale) {
  const float gap = kElementGap * scale;
  std::array<std::vector<Slot>, 3> g;
  for (std::size_t a = 0; a < 3; ++a) {
    g[a].reserve(groups[a].size());
    for (KeyItem* it : groups[a]) {
      const KeyElement& e = content[it->index];
      const float w = e.image ? image_width(*e.image, it->text.size) : c.measure(e.text, it->text);
      g[a].push_back(Slot{it, e.text, w, w, w});
    }
  }
  std::vector<Slot*> all;
  for (auto& grp : g)
    for (Slot& s : grp) all.push_back(&s);
  if (all.empty()) return;

  const float avail = std::max(0.f, row.w - gap * static_cast<float>(all.size() - 1));
  float natural = 0;
  for (const Slot* s : all) natural += s->natural;
  if (natural > avail) {
    if (all.size() == 1) all[0]->share = avail;  // exactly the width fit_text uses
    else share_out(all, avail);
    for (Slot* s : all) {
      if (s->natural <= s->share) continue;
      if (const auto& img = content[s->item->index].image) {  // an image shrinks to its share
        s->width = s->share;
        s->item->text.size = s->share * static_cast<float>(img->height) / static_cast<float>(img->width);
        continue;
      }
      shrink_to_fit(c, s->text, s->item->text, s->share, scaled_min(s->item->spec->min_size, scale));
      const float w = c.measure(s->text, s->item->text);
      s->width = std::min(w, s->share);
      if (w > s->share) s->item->clip = RectF{0, row.y, s->share, row.h};  // x set below
    }
  }

  auto width_of = [gap](const std::vector<Slot>& grp) {
    float w = 0;
    for (std::size_t i = 0; i < grp.size(); ++i) w += grp[i].width + (i ? gap : 0.f);
    return w;
  };
  const float L = width_of(g[0]), C = width_of(g[1]), R = width_of(g[2]);
  const float lo = row.x + (g[0].empty() ? 0.f : L + gap);
  const float hi = row.x + row.w - (g[2].empty() ? 0.f : R + gap);
  for (std::size_t a = 0; a < 3; ++a) {
    auto& grp = g[a];
    if (grp.empty()) continue;
    if (grp.size() == 1) {  // one text: drawn by its alignment in its box, like fit_text
      Slot& s = grp[0];
      KeyItem& it = *s.item;
      it.text.align = static_cast<HAlign>(a);
      it.text.valign = VAlign::Center;
      if (a == 0) it.rect = RectF{row.x, row.y, s.width, row.h};
      else if (a == 2) it.rect = RectF{row.x + row.w - s.width, row.y, s.width, row.h};
      else it.rect = RectF{lo, row.y, std::max(0.f, hi - lo), row.h};
      if (it.clip.w > 0)
        it.clip.x = a == 0 ? row.x : a == 2 ? row.x + row.w - it.clip.w : lo + (hi - lo - it.clip.w) / 2;
      continue;
    }
    // The centre group, whatever its size, is centred between the left and right groups, like a lone centre text.
    float x = a == 0 ? row.x : a == 2 ? row.x + row.w - R : lo + std::max(0.f, (hi - lo - C) / 2);
    for (Slot& s : grp) {  // side by side, in declaration order
      KeyItem& it = *s.item;
      it.text.align = HAlign::Left;
      it.text.valign = VAlign::Center;
      it.rect = RectF{x, row.y, s.width, row.h};
      if (it.clip.w > 0) it.clip.x = x;
      x += s.width + gap;
    }
  }
}

// Lays out the texts of `row` in `items` (graphs take their whole row, as placed).
void place(Canvas& c, std::vector<KeyItem>& items, Row row, std::span<const KeyElement> content,
           std::span<const ElementSpec> specs, const KeyTemplate& tmpl) {
  std::array<std::vector<KeyItem*>, 3> groups;
  for (KeyItem& it : items)  // `items` no longer grows: the pointers stay valid
    if (it.style.row == row && it.spec->kind != ElementKind::Graph)
      groups[static_cast<std::size_t>(it.style.align)].push_back(&it);
  for (auto& grp : groups)  // declaration order, whatever the content order
    std::stable_sort(grp.begin(), grp.end(), [specs](const KeyItem* x, const KeyItem* y) {
      return x->spec - specs.data() < y->spec - specs.data();
    });
  place_row(c, tmpl.rect(row), groups, content, tmpl.scale);
}

// Two-line keys: the two lines show two equal values, so they keep one size. A text of
// Line1 pairs with the text of Line2 of the same role and rank among that role's shown texts, in
// declaration order: symbol with symbol, price with price, even when the user hides one symbol. A
// pair shares the smaller fitted size when both ask for the same size (the user did not size them
// apart), as the prices always did. A row whose sizes changed is laid out again from them, so
// its boxes and gaps match the final widths.
void share_two_line_sizes(Canvas& c, std::vector<KeyItem>& items, std::span<const KeyElement> content,
                          std::span<const ElementSpec> specs, const KeyTemplate& tmpl) {
  auto texts_of = [&](Row row, TextRole role) {
    std::vector<KeyItem*> out;
    for (KeyItem& it : items)
      if (it.style.row == row && it.spec->kind != ElementKind::Graph && it.spec->role == role) out.push_back(&it);
    std::stable_sort(out.begin(), out.end(), [specs](const KeyItem* x, const KeyItem* y) {
      return x->spec - specs.data() < y->spec - specs.data();
    });
    return out;
  };
  std::vector<std::pair<KeyItem*, float>> shared;  // a paired text and its shared size
  for (TextRole role : {TextRole::None, TextRole::Label, TextRole::Value, TextRole::LongValue, TextRole::TwoLine,
                        TextRole::Secondary, TextRole::Symbol}) {
    const std::vector<KeyItem*> one = texts_of(Row::Line1, role), two = texts_of(Row::Line2, role);
    for (std::size_t i = 0; i < std::min(one.size(), two.size()); ++i) {
      if (one[i]->style.size != two[i]->style.size) continue;
      const float size = std::min(one[i]->text.size, two[i]->text.size);
      shared.emplace_back(one[i], size);
      shared.emplace_back(two[i], size);
    }
  }
  for (Row row : {Row::Line1, Row::Line2}) {
    bool changed = false;
    for (auto [it, size] : shared) changed |= it->style.row == row && it->text.size != size;
    if (!changed) continue;  // the row's layout stands
    for (KeyItem& it : items) {
      if (it.style.row != row || it.spec->kind == ElementKind::Graph) continue;
      it.text.size = it.style.size;  // fitted again from the resolved size...
      it.clip = RectF{};
    }
    for (auto [it, size] : shared)
      if (it->style.row == row) it->text.size = size;  // ...or from the shared one
    place(c, items, row, content, specs, tmpl);
  }
}

}  // namespace

std::vector<KeyItem> layout_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs,
                                const KeyTemplate& tmpl) {
  std::vector<KeyItem> items;
  items.reserve(content.size());
  for (std::size_t i = 0; i < content.size(); ++i) {
    const KeyElement& e = content[i];
    const ElementSpec* spec = find_spec(specs, e.key);
    if (!spec) continue;  // not declared by the module: never drawn
    const bool graph = spec->kind == ElementKind::Graph;
    if (!shows(*spec, e)) continue;  // nothing to show
    KeyItem it;
    it.index = i;
    it.spec = spec;
    it.style = c.resolve(*spec, input_of(e));
    if (!it.style.visible) continue;  // a hidden element frees its place
    it.style.size *= tmpl.scale;  // sizes are declared at 112 px (x 1 there: the same value)
    it.rect = tmpl.rect(it.style.row);
    if (!graph) it.text = c.style(*spec, it.style);
    if (spec->kind == ElementKind::Icon && !e.font.empty()) it.text.font = e.font;  // an icon font's glyph
    items.push_back(it);
  }
  for (Row row : {Row::Top, Row::Middle, Row::Bottom, Row::Line1, Row::Line2}) place(c, items, row, content, specs, tmpl);
  share_two_line_sizes(c, items, content, specs, tmpl);
  return items;
}

void render_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs,
                const KeyTemplate& tmpl) {
  for (const KeyItem& it : layout_key(c, content, specs, tmpl)) {
    const KeyElement& e = content[it.index];
    if (it.spec->kind == ElementKind::Graph) {
      draw_graph(c, e.points, e.capacity, it.rect, it.style.color);
    } else if (it.spec->kind == ElementKind::Icon && e.image) {
      c.image(*e.image, image_rect(*e.image, it));
    } else if (it.clip.w > 0) {
      ClipGuard guard(c, it.clip);
      c.text(e.text, it.rect, it.text);
    } else {
      c.text(e.text, it.rect, it.text);
    }
  }
}

std::vector<KeyItem> layout_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs) {
  return layout_key(c, content, specs, key_template(c.width(), c.height()));
}

void render_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs) {
  render_key(c, content, specs, key_template(c.width(), c.height()));
}

void draw_graph(Canvas& c, std::span<const float> v, std::size_t capacity, RectF area, Rgb color) {
  if (capacity == 0) capacity = v.size();
  if (v.size() < 2 || capacity < 2) return;
  if (v.size() > capacity) v = v.last(capacity);
  const float step = area.w / static_cast<float>(capacity - 1);
  const float x0 = area.x + area.w - step * static_cast<float>(v.size() - 1);
  std::vector<PointF> pts;
  pts.reserve(v.size());
  for (std::size_t i = 0; i < v.size(); ++i) {
    const float y = std::isnan(v[i]) ? 0.f : std::clamp(v[i], 0.f, 1.f);
    pts.push_back(PointF{x0 + step * static_cast<float>(i), area.y + area.h * (1.f - y)});
  }
  c.polyline(pts, color, 2.f);
}

}  // namespace ld
