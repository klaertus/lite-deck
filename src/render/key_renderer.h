// src/render/key_renderer.h: the key engine. A key module says what to show (KeyElement);
// this places each element in its row, by alignment, fits it, colours it and draws it.
#pragma once
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "render/renderer.h"

namespace ld {

// The rows of a key, defined at 112 x 112 px (6 px side margins) and scaled by height / 112: a
// non-square key takes its row heights from its height and spreads its rows across its width.
struct KeyTemplate {
  RectF top, middle, bottom, line1, line2;
  float scale = 1;  // key height / 112: element sizes, minimum sizes and gaps follow it
  RectF rect(Row row) const;
};
inline constexpr float kKeyBase = 112;
inline constexpr KeyTemplate kKeyTemplate{RectF{6, 4, 100, 20}, RectF{6, 24, 100, 52}, RectF{6, 76, 100, 32},
                                          RectF{6, 8, 100, 46}, RectF{6, 58, 100, 46}};
// The template of a width x height key; key_template(112, 112) is kKeyTemplate, value for value.
KeyTemplate key_template(float width, float height);

// Space between two texts of a row (side by side, or two alignment groups).
inline constexpr float kElementGap = 4;

// One element of what a key shows. Its kind comes from the module's ElementSpec.
struct KeyElement {
  std::string key;                   // an ElementSpec key of the module; others are never drawn
  std::wstring text;                 // Text and Icon; empty = not shown
  std::vector<float> points;         // Graph: 0..1 (clamped), oldest first; fewer than 2 = not shown
  std::size_t capacity = 0;          // Graph: points spanning the whole width (0 = points.size())
  ElementState state = ElementState::Normal;
  std::optional<double> metric;      // for a gradient
  std::optional<Gradient> gradient;  // the module's scale for `metric`
  std::optional<Rgb> color;          // the module's default colour (e.g. per gpu mode); the user's wins
  std::optional<float> size;         // the module's default size; the user's wins
  // Icon only: a picture instead of `text` (a program's icon, an image file), drawn `size` px high
  // with its proportions, aligned and fitted like a text of that width.
  std::shared_ptr<const Bitmap> image;
  // Icon only: the font of `text`, for a glyph of an icon font; "" = Segoe UI (colour emoji).
  std::wstring font;
};

// An element placed by layout_key.
struct KeyItem {
  std::size_t index = 0;  // in the content
  const ElementSpec* spec = nullptr;
  ResolvedStyle style;    // after the user's settings and the state
  TextStyle text;         // Text, Icon: fitted size, weight, colour, alignment
  RectF rect;             // where it is drawn (a graph: its whole row)
  RectF clip{};           // w > 0: still too wide at its minimum size, drawn clipped to this box
};

// (v - lo) / (hi - lo) for each value, oldest first: the points of a graph. Empty when hi <= lo
// (or NaN): the graph is not drawn, as the plan-4 sparkline did.
std::vector<float> graph_points(const std::deque<float>& values, float lo, float hi);

// Places the visible, non-empty elements of `content`:
// - each text in the row of its resolved style, grouped by alignment: the left group from the
//   left edge, the right group to the right edge, the centre group centred between them;
// - several texts of one group side by side, in declaration order (specs), kElementGap apart;
//   the centre group is centred in the space left between the left and right groups;
// - when the row is too narrow, a text below an equal share keeps its width and the others split
//   the rest; a text wider than its share shrinks (never below its minimum), then is clipped to it;
// - every text is centred vertically in its row; a graph takes its whole row;
// - on two-line keys, a text of Line1 and the text of Line2 of the same role and rank (declaration
//   order, among the shown texts) keep one size: the smaller fitted one, when both ask for the same
//   size; a row whose sizes changed is laid out again from them.
// One text alone in its group is laid out exactly as fit_text in the row (same pixels).
// An icon with an image is laid out as a text as wide as the image at its size; when its share is
// narrower, the image shrinks to it (never clipped).
// The same with the template of the canvas size: key_template(c.width(), c.height()).
std::vector<KeyItem> layout_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs);
// Sizes (declared at 112 px) are multiplied by tmpl.scale.
std::vector<KeyItem> layout_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs,
                                const KeyTemplate& tmpl);
// layout_key, then draws the items in content order.
void render_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs);
void render_key(Canvas& c, std::span<const KeyElement> content, std::span<const ElementSpec> specs,
                const KeyTemplate& tmpl);
// A 2 px line through `points`, newest at the right edge; `capacity` points span the whole width.
void draw_graph(Canvas& c, std::span<const float> points, std::size_t capacity, RectF area, Rgb color);

}  // namespace ld
