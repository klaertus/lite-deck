// src/render/renderer.h
#pragma once
#include <d2d1.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <compare>
#include <cstdint>
#include <list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include "core/image.h"
#include "core/types.h"
#include "render/look.h"

namespace ld {

using Microsoft::WRL::ComPtr;

struct RectF { float x = 0, y = 0, w = 0, h = 0; };
struct PointF { float x = 0, y = 0; };

// A small image a module owns, such as a program's icon: premultiplied BGRA, rows of width * 4
// bytes. Drawn with Canvas::image.
struct Bitmap {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> bgra;
  std::size_t bytes() const { return bgra.size(); }
};

struct TextStyle {
  std::wstring font;  // "" = the slot's font and italic (Canvas::set_look); Segoe UI by default
  float size = 20;
  bool bold = false;
  int weight = 0;  // DirectWrite weight (e.g. 600 = semibold); 0 = from `bold`
  Rgb color{255, 255, 255};
  HAlign align = HAlign::Center;
  VAlign valign = VAlign::Center;
  bool slot_italic = false;  // with a named `font` (a role's): the slot's italic still applies (Canvas)
};

// Shared factories and cached text formats. COM must be initialized on the calling thread.
class Renderer {
public:
  Renderer();
  ID2D1Factory* d2d() const { return d2d_.Get(); }
  IDWriteFactory* dwrite() const { return dwrite_.Get(); }
  IDWriteFactory2* dwrite2() const { return dwrite2_.Get(); }
  IWICImagingFactory* wic() const { return wic_.Get(); }
  // `font` and `italic` are already resolved (Canvas turns an empty TextStyle::font into the slot's).
  IDWriteTextFormat* text_format(const TextStyle& style, std::wstring_view font, bool italic);
  IDWriteTextFormat* text_format(const TextStyle& style) {
    return text_format(style, style.font.empty() ? std::wstring_view(kDefaultFont) : std::wstring_view(style.font),
                       false);
  }
  std::size_t cached_formats() const { return formats_.size(); }
  static constexpr std::size_t kMaxFormats = 48;
  // A no-wrap layout of `text` in a w x h box, aligned per `style` (its color is not part of it).
  // Shaping is the costly part of drawing text, and animated text (a marquee) draws the same string
  // at a new position every frame: layouts are kept in a small LRU cache keyed by the text and all
  // that shapes it, never by the position. Do not modify a returned layout: it is shared.
  ComPtr<IDWriteTextLayout> text_layout(std::wstring_view text, const TextStyle& style, std::wstring_view font, bool italic,
                                        float w, float h);
  std::size_t cached_layouts() const { return layout_lru_.size(); }
  std::uint64_t layout_hits() const { return layout_hits_; }
  std::uint64_t layout_misses() const { return layout_misses_; }
  static constexpr std::size_t kMaxLayouts = 64;
  // A filled triangle's path geometry (the play icon, drawn every frame): built once per corner set.
  ID2D1PathGeometry* triangle(PointF a, PointF b, PointF c);
  std::size_t cached_triangles() const { return triangles_.size(); }
  static constexpr std::size_t kMaxTriangles = 16;

private:
  ComPtr<ID2D1Factory> d2d_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IDWriteFactory2> dwrite2_;  // TranslateColorGlyphRun (color emoji as layers)
  ComPtr<IWICImagingFactory> wic_;
  std::map<std::tuple<std::wstring, float, int, bool>, ComPtr<IDWriteTextFormat>> formats_;
  struct LayoutKey {
    std::wstring text, font;
    float size;  // snapped, as the format's
    int weight;
    bool italic;
    HAlign align;
    VAlign valign;
    float w, h;
    auto operator<=>(const LayoutKey&) const = default;
  };
  struct LayoutEntry {
    LayoutKey key;
    ComPtr<IDWriteTextLayout> layout;  // holds its own reference to the format
  };
  std::list<LayoutEntry> layout_lru_;  // most recently used first
  std::map<LayoutKey, std::list<LayoutEntry>::iterator> layouts_;
  std::uint64_t layout_hits_ = 0, layout_misses_ = 0;
  std::map<std::array<float, 6>, ComPtr<ID2D1PathGeometry>> triangles_;
  ComPtr<ID2D1PathGeometry> nan_triangle_;
};

class Canvas;

// An off-screen BGRA bitmap with a software Direct2D target. Reusable across frames.
class Surface {
public:
  Surface(Renderer& renderer, int width, int height);
  int width() const { return w_; }
  int height() const { return h_; }
  Canvas canvas();
  Canvas canvas(RectF region);
  // The test reference for the frames sent to the device: production encodes lent pixels with a
  // JpegEncoder (render/jpeg.h), which gives the same bytes; the tests pin both against each other.
  std::vector<std::uint8_t> encode_jpeg(int quality, bool rotate180);
  Rgb pixel(int x, int y);
  // The whole bitmap, BGRA (premultiplied; opaque after a clear), rows of width * 4 bytes.
  std::vector<std::uint8_t> pixels();

private:
  friend class Canvas;
  friend class SurfacePixels;
  void begin();
  void end();
  Renderer& r_;
  int w_, h_;
  ComPtr<IWICBitmap> bmp_;
  ComPtr<ID2D1RenderTarget> rt_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<ID2D1SolidColorBrush> layer_brush_;  // color font layers
  // Edge fades (marquee): one gradient brush per background colour, bounded (a flash blends many).
  ID2D1LinearGradientBrush* fade_brush(Rgb color);
  std::map<std::uint32_t, ComPtr<ID2D1LinearGradientBrush>> fade_brushes_;
  static constexpr std::size_t kMaxFadeBrushes = 16;
  bool drawing_ = false;
};

// Read access to a Surface's pixels without a copy, for as long as it lives: no drawing on the surface
// meanwhile. What the Deck lends to its sink, and the previews to their encoder.
class SurfacePixels {
public:
  explicit SurfacePixels(Surface& surface);
  SurfacePixels(const SurfacePixels&) = delete;
  SurfacePixels& operator=(const SurfacePixels&) = delete;
  const Image& image() const { return image_; }

private:
  ComPtr<IWICBitmapLock> lock_;
  Image image_;
};

// Drawing view on a region of a Surface: coordinates are region-relative and clipped to it.
class Canvas {
public:
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  Canvas(Canvas&& other) noexcept;
  ~Canvas();

  float width() const { return region_.w; }
  float height() const { return region_.h; }
  void clear(Rgb color);
  void fill_rect(RectF rect, Rgb color, float alpha = 1.f);
  void line(PointF a, PointF b, Rgb color, float stroke = 1.f);
  void polyline(std::span<const PointF> points, Rgb color, float stroke = 1.f);
  void fill_round_rect(RectF rect, float radius, Rgb color);
  void fill_triangle(PointF a, PointF b, PointF c, Rgb color);
  // Fills `rect` with `color` in a smooth horizontal fade: opaque on its left side when
  // `opaque_left` (else on its right), fully transparent on the other side.
  void fade_rect(RectF rect, Rgb color, bool opaque_left);
  // Restricts drawing to `rect` until the matching pop_clip (nests inside the canvas region clip).
  void push_clip(RectF rect);
  void pop_clip();
  void text(std::wstring_view text, RectF rect, const TextStyle& style);
  // Draws `bitmap` scaled to `rect` (a high-quality resampling, then a pixel-aligned copy). A
  // malformed bitmap or an empty rect draws nothing.
  void image(const Bitmap& bitmap, RectF rect);
  float measure(std::wstring_view text, const TextStyle& style);
  // The slot's appearance, set by Deck::render_slot: the font and italic of every TextStyle whose
  // font is empty, and the user's element sizes and weights. nullptr = Segoe UI, upright, the
  // modules' defaults. `look` must outlive the drawing.
  void set_look(const SlotLook* look) { look_ = look; }
  const SlotLook& look() const;
  // The TextStyle of one element: its default size (or `default_size`, when the module computes it
  // from the slot height) and weight, then the slot's overrides. Icons name Segoe UI, so they stay
  // upright and in colour whatever the slot font, and ignore a weight override. Same path as resolve.
  TextStyle style(const ElementSpec& element) const { return style(element, element.size); }
  TextStyle style(const ElementSpec& element, float default_size) const;
  // One element as this slot shows it (render/resolve.h): the module's defaults in `in`, then the
  // slot's role for the element's TextRole, then the user's settings for this slot, then the state,
  // coloured with the slot's palette.
  ResolvedStyle resolve(const ElementSpec& element, const ElementInput& in = {}) const;
  // The TextStyle of a resolved element: size, weight and colour; icons name Segoe UI; an element
  // with a role font names it and keeps the slot's italic.
  TextStyle style(const ElementSpec& element, const ResolvedStyle& resolved) const;

private:
  friend class Surface;
  Canvas(Surface& surface, RectF region);
  ID2D1SolidColorBrush* brush(Rgb color, float alpha);
  ComPtr<IDWriteTextLayout> layout(std::wstring_view text, float w, float h, const TextStyle& style);
  Surface* s_;
  RectF region_;
  const SlotLook* look_ = nullptr;
};

// Scoped push_clip/pop_clip: the pop happens even when drawing throws.
class ClipGuard {
public:
  ClipGuard(Canvas& c, RectF rect) : c_(c) { c_.push_clip(rect); }
  ~ClipGuard() { c_.pop_clip(); }
  ClipGuard(const ClipGuard&) = delete;
  ClipGuard& operator=(const ClipGuard&) = delete;

private:
  Canvas& c_;
};

}  // namespace ld
