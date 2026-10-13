// src/render/renderer.cpp
#include "render/renderer.h"
#include "render/resolve.h"
#include "render/jpeg.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ld {

static void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::format("{} failed: 0x{:08x}", what, static_cast<unsigned long>(hr)));
}

static D2D1_COLOR_F to_d2d(Rgb c, float alpha) {
  return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, alpha);
}

Renderer::Renderer() {
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()), "D2D1CreateFactory");
  check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())), "DWriteCreateFactory");
  check(dwrite_.As(&dwrite2_), "IDWriteFactory2");
  check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)),
        "CoCreateInstance(WIC)");
}

static int resolved_weight(const TextStyle& s) {
  return s.weight > 0 ? s.weight : s.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
}

// Sizes are snapped to half a pixel so animated sizes reuse a few formats, and the cache is bounded:
// each format pins DirectWrite glyph caches, so an unbounded map grows with every new size (and
// now with every font and style the slots use).
static float snapped_size(const TextStyle& s) { return std::max(1.f, std::round(s.size * 2.f) / 2.f); }

IDWriteTextFormat* Renderer::text_format(const TextStyle& s, std::wstring_view font, bool italic) {
  const int weight = resolved_weight(s);
  const float size = snapped_size(s);
  auto key = std::make_tuple(std::wstring(font), size, weight, italic);
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.Get();
  if (formats_.size() >= kMaxFormats) formats_.clear();  // layouts in flight hold their own reference
  ComPtr<IDWriteTextFormat> f;
  check(dwrite_->CreateTextFormat(std::get<0>(key).c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                                  italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f),
        "CreateTextFormat");
  return (formats_[key] = f).Get();
}

ComPtr<IDWriteTextLayout> Renderer::text_layout(std::wstring_view text, const TextStyle& st, std::wstring_view font,
                                                bool italic, float w, float h) {
  // DirectWrite rejects a negative or NaN box (E_INVALIDARG): such a box is an empty one, never an error.
  if (!(w >= 0)) w = 0;
  if (!(h >= 0)) h = 0;
  LayoutKey key{std::wstring(text), std::wstring(font), snapped_size(st), resolved_weight(st), italic, st.align, st.valign, w, h};
  if (auto it = layouts_.find(key); it != layouts_.end()) {
    ++layout_hits_;
    layout_lru_.splice(layout_lru_.begin(), layout_lru_, it->second);
    return it->second->layout;
  }
  ++layout_misses_;
  IDWriteTextFormat* format = text_format(st, font, italic);
  ComPtr<IDWriteTextLayout> l;
  check(dwrite_->CreateTextLayout(key.text.data(), static_cast<UINT32>(key.text.size()), format, w, h, &l),
        "CreateTextLayout");
  l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  l->SetTextAlignment(st.align == HAlign::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                      : st.align == HAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                  : DWRITE_TEXT_ALIGNMENT_CENTER);
  l->SetParagraphAlignment(st.valign == VAlign::Top ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                           : st.valign == VAlign::Bottom ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
                                                         : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  if (layout_lru_.size() >= kMaxLayouts) {
    layouts_.erase(layout_lru_.back().key);
    layout_lru_.pop_back();
  }
  layout_lru_.push_front(LayoutEntry{std::move(key), l});
  layouts_.emplace(layout_lru_.front().key, layout_lru_.begin());
  return l;
}

ID2D1PathGeometry* Renderer::triangle(PointF a, PointF b, PointF c) {
  const std::array<float, 6> key{a.x, a.y, b.x, b.y, c.x, c.y};
  if (auto it = triangles_.find(key); it != triangles_.end()) return it->second.Get();
  ComPtr<ID2D1PathGeometry> geo;
  check(d2d_->CreatePathGeometry(&geo), "CreatePathGeometry");
  ComPtr<ID2D1GeometrySink> sink;
  check(geo->Open(&sink), "PathGeometry.Open");
  sink->BeginFigure(D2D1::Point2F(a.x, a.y), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(D2D1::Point2F(b.x, b.y));
  sink->AddLine(D2D1::Point2F(c.x, c.y));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  check(sink->Close(), "GeometrySink.Close");
  if (std::any_of(key.begin(), key.end(), [](float v) { return std::isnan(v); })) {
    nan_triangle_ = geo;  // no ordering for NaN: kept apart, alive until the next one
    return geo.Get();
  }
  if (triangles_.size() >= kMaxTriangles) triangles_.clear();
  return (triangles_[key] = geo).Get();
}

Surface::Surface(Renderer& r, int w, int h) : r_(r), w_(w), h_(h) {
  check(r_.wic()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp_), "CreateBitmap");
  auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  check(r_.d2d()->CreateWicBitmapRenderTarget(bmp_.Get(), props, &rt_), "CreateWicBitmapRenderTarget");
  rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  check(rt_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_), "CreateSolidColorBrush");
  check(rt_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &layer_brush_), "CreateSolidColorBrush");
}

void Surface::begin() {
  if (drawing_) return;
  rt_->BeginDraw();
  drawing_ = true;
}

void Surface::end() {
  if (!drawing_) return;
  drawing_ = false;
  check(rt_->EndDraw(), "EndDraw");
}

Canvas Surface::canvas() { return Canvas(*this, RectF{0, 0, static_cast<float>(w_), static_cast<float>(h_)}); }
Canvas Surface::canvas(RectF region) { return Canvas(*this, region); }

Rgb Surface::pixel(int x, int y) {
  end();
  WICRect rc{x, y, 1, 1};
  ComPtr<IWICBitmapLock> lock;
  check(bmp_->Lock(&rc, WICBitmapLockRead, &lock), "Lock");
  UINT size = 0;
  BYTE* p = nullptr;
  check(lock->GetDataPointer(&size, &p), "GetDataPointer");
  if (!p || size < 4) throw std::runtime_error("GetDataPointer returned no pixel");
  return Rgb{p[2], p[1], p[0]};
}

std::vector<std::uint8_t> Surface::pixels() {
  end();
  WICRect rc{0, 0, w_, h_};
  ComPtr<IWICBitmapLock> lock;
  check(bmp_->Lock(&rc, WICBitmapLockRead, &lock), "Lock");
  UINT stride = 0;
  check(lock->GetStride(&stride), "GetStride");
  UINT size = 0;
  BYTE* p = nullptr;
  check(lock->GetDataPointer(&size, &p), "GetDataPointer");
  const std::size_t row = static_cast<std::size_t>(w_) * 4;
  if (!p || stride < row || size < static_cast<std::size_t>(stride) * (h_ - 1) + row)
    throw std::runtime_error("GetDataPointer returned a short buffer");
  std::vector<std::uint8_t> out(row * h_);
  for (int y = 0; y < h_; ++y) memcpy(out.data() + row * y, p + static_cast<std::size_t>(stride) * y, row);
  return out;
}

SurfacePixels::SurfacePixels(Surface& s) {
  s.end();
  WICRect rc{0, 0, s.w_, s.h_};
  check(s.bmp_->Lock(&rc, WICBitmapLockRead, &lock_), "Lock");
  UINT stride = 0;
  check(lock_->GetStride(&stride), "GetStride");
  UINT size = 0;
  BYTE* p = nullptr;
  check(lock_->GetDataPointer(&size, &p), "GetDataPointer");
  const std::size_t row = static_cast<std::size_t>(s.w_) * 4;
  if (!p || stride < row || size < static_cast<std::size_t>(stride) * (s.h_ - 1) + row)
    throw std::runtime_error("GetDataPointer returned a short buffer");
  image_ = Image{s.w_, s.h_, static_cast<int>(stride), std::span<const std::uint8_t>(p, size)};
}

std::vector<std::uint8_t> Surface::encode_jpeg(int quality, bool rotate180) {
  end();
  return encode_jpeg_source(r_.wic(), bmp_.Get(), w_, h_, quality, rotate180 ? Orientation::Rotate180 : Orientation::None);
}

Canvas::Canvas(Surface& s, RectF region) : s_(&s), region_(region) {
  s_->begin();
  s_->rt_->SetTransform(D2D1::Matrix3x2F::Translation(region.x, region.y));
  s_->rt_->PushAxisAlignedClip(D2D1::RectF(0, 0, region.w, region.h), D2D1_ANTIALIAS_MODE_ALIASED);
}

Canvas::Canvas(Canvas&& o) noexcept : s_(o.s_), region_(o.region_), look_(o.look_) { o.s_ = nullptr; }

Canvas::~Canvas() {
  if (!s_) return;
  s_->rt_->PopAxisAlignedClip();
  s_->rt_->SetTransform(D2D1::Matrix3x2F::Identity());
}

ID2D1SolidColorBrush* Canvas::brush(Rgb c, float alpha) {
  s_->brush_->SetColor(to_d2d(c, alpha));
  return s_->brush_.Get();
}

void Canvas::clear(Rgb c) { fill_rect(RectF{0, 0, region_.w, region_.h}, c); }

void Canvas::fill_rect(RectF r, Rgb c, float alpha) {
  s_->rt_->FillRectangle(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), brush(c, alpha));
}

void Canvas::line(PointF a, PointF b, Rgb c, float stroke) {
  s_->rt_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), brush(c, 1.f), stroke);
}

void Canvas::polyline(std::span<const PointF> pts, Rgb c, float stroke) {
  for (std::size_t i = 1; i < pts.size(); ++i) line(pts[i - 1], pts[i], c, stroke);
}

void Canvas::fill_round_rect(RectF r, float radius, Rgb c) {
  s_->rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), radius, radius),
                                brush(c, 1.f));
}

void Canvas::fill_triangle(PointF a, PointF b, PointF c, Rgb color) {
  s_->rt_->FillGeometry(s_->r_.triangle(a, b, c), brush(color, 1.f));
}

ID2D1LinearGradientBrush* Surface::fade_brush(Rgb c) {
  const std::uint32_t key = (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b;
  if (auto it = fade_brushes_.find(key); it != fade_brushes_.end()) return it->second.Get();
  if (fade_brushes_.size() >= kMaxFadeBrushes) fade_brushes_.clear();
  const D2D1_GRADIENT_STOP stops[] = {{0.f, to_d2d(c, 1.f)}, {1.f, to_d2d(c, 0.f)}};
  ComPtr<ID2D1GradientStopCollection> coll;
  check(rt_->CreateGradientStopCollection(stops, 2, &coll), "CreateGradientStopCollection");
  ComPtr<ID2D1LinearGradientBrush> b;
  check(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)),
                                       coll.Get(), &b),
        "CreateLinearGradientBrush");
  return (fade_brushes_[key] = b).Get();
}

void Canvas::fade_rect(RectF r, Rgb c, bool opaque_left) {
  if (!(r.w > 0) || !(r.h > 0)) return;
  ID2D1LinearGradientBrush* b = s_->fade_brush(c);
  const float l = r.x, rt = r.x + r.w;
  b->SetStartPoint(D2D1::Point2F(opaque_left ? l : rt, r.y));  // stop 0: opaque
  b->SetEndPoint(D2D1::Point2F(opaque_left ? rt : l, r.y));
  s_->rt_->FillRectangle(D2D1::RectF(l, r.y, rt, r.y + r.h), b);
}

void Canvas::push_clip(RectF r) {
  s_->rt_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::pop_clip() { s_->rt_->PopAxisAlignedClip(); }

const SlotLook& Canvas::look() const {
  static const SlotLook kDefault;
  return look_ ? *look_ : kDefault;
}

TextStyle Canvas::style(const ElementSpec& e, float default_size) const {
  ElementInput in;
  in.size = default_size;
  return style(e, resolve(e, in));
}

ResolvedStyle Canvas::resolve(const ElementSpec& e, const ElementInput& in) const {
  const SlotLook& l = look();
  auto it = l.elements.find(std::string_view(e.key));
  auto role = e.role == TextRole::None ? l.roles.end() : l.roles.find(e.role);
  return ld::resolve(e, it == l.elements.end() ? nullptr : &it->second, in, l.states,
                     role == l.roles.end() ? nullptr : &role->second);
}

TextStyle Canvas::style(const ElementSpec& e, const ResolvedStyle& r) const {
  TextStyle st;
  st.size = r.size;
  st.weight = r.weight;
  st.color = r.color;
  if (e.icon || e.kind == ElementKind::Icon) {
    st.font = kDefaultFont;  // explicit: never the slot font, never italic
  } else if (!r.font.empty()) {
    st.font = r.font;  // a role's font, slanted like the rest of the slot
    st.slot_italic = true;
  }
  return st;
}

ComPtr<IDWriteTextLayout> Canvas::layout(std::wstring_view text, float w, float h, const TextStyle& st) {
  const SlotLook& look = this->look();
  const bool inherit = st.font.empty();
  return s_->r_.text_layout(text, st, inherit ? std::wstring_view(look.font) : std::wstring_view(st.font),
                            (inherit || st.slot_italic) && look.italic, w, h);
}

namespace {

// Draws a text layout like DrawTextLayout, but color fonts (emoji) as their COLR layers: each layer is
// a plain glyph run in its palette color. D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT renders them on a
// path whose glyph cache costs ~10 MB at the first emoji and ~100 KB per new emoji and size, never
// given back (measured in tests/test_stress_memory.cpp); plain glyph runs cost a few KB each.
// The COLR v0 layers are flat colors: no gradients, unlike the COLR v1 drawing of recent Windows.
class LayerTextRenderer final : public IDWriteTextRenderer {
public:
  LayerTextRenderer(ID2D1RenderTarget* rt, IDWriteFactory2* f, ID2D1SolidColorBrush* text, ID2D1SolidColorBrush* layer)
      : rt_(rt), f_(f), text_(text), layer_(layer) {}
  // Lives on the stack for one Draw call: no reference counting.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer)) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
    *disabled = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
    rt_->GetTransform(reinterpret_cast<D2D1_MATRIX_3X2_F*>(m));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override {
    FLOAT dx = 96, dy = 96;
    rt_->GetDpi(&dx, &dy);
    *ppd = dx / 96.f;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                         const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown*) override {
    // A font problem must never fail the draw: fall back to the plain run.
    ComPtr<IDWriteFontFace2> face2;
    if (FAILED(run->fontFace->QueryInterface(IID_PPV_ARGS(&face2))) || !face2->IsColorFont()) {
      rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, text_, mode);
      return S_OK;
    }
    ComPtr<IDWriteColorGlyphRunEnumerator> layers;
    HRESULT hr = f_->TranslateColorGlyphRun(x, y, run, desc, mode, nullptr, 0, &layers);
    if (FAILED(hr)) {  // DWRITE_E_NOCOLOR or any other failure
      rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, text_, mode);
      return S_OK;
    }
    bool drawn = false;
    BOOL more = FALSE;
    while (true) {
      if (FAILED(layers->MoveNext(&more)) || !more) break;
      const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
      if (FAILED(layers->GetCurrentRun(&layer))) break;
      ID2D1SolidColorBrush* b = text_;  // 0xFFFF: the layer takes the text color
      if (layer->paletteIndex != 0xFFFF) {
        layer_->SetColor(D2D1::ColorF(layer->runColor.r, layer->runColor.g, layer->runColor.b, layer->runColor.a));
        b = layer_;
      }
      rt_->DrawGlyphRun(D2D1::Point2F(layer->baselineOriginX, layer->baselineOriginY), &layer->glyphRun, b, mode);
      drawn = true;
    }
    if (!drawn) rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, text_, mode);
    return S_OK;
  }
  // Decorations and inline objects are intentionally no-ops: no text in lite-deck uses them. Implement if that changes.
  HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
  // (no-op, see DrawUnderline)
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
    return S_OK;
  }
  // (no-op, see DrawUnderline)
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
    return S_OK;
  }

private:
  ID2D1RenderTarget* rt_;
  IDWriteFactory2* f_;
  ID2D1SolidColorBrush* text_;
  ID2D1SolidColorBrush* layer_;
};

}  // namespace

void Canvas::text(std::wstring_view text, RectF r, const TextStyle& st) {
  // A box computed from a 0/0 or an overflow draws nothing; a negative width or height is an empty
  // box (text_layout). Neither may fail the frame: the slot would show "!" until the next reload.
  if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h)) return;
  if (!std::isfinite(st.size)) return;
  auto l = layout(text, r.w, r.h, st);
  LayerTextRenderer renderer(s_->rt_.Get(), s_->r_.dwrite2(), brush(st.color, 1.f), s_->layer_brush_.Get());
  check(l->Draw(nullptr, &renderer, r.x, r.y), "TextLayout.Draw");
}

void Canvas::image(const Bitmap& b, RectF r) {
  if (b.width <= 0 || b.height <= 0 || b.bgra.size() < static_cast<std::size_t>(b.width) * b.height * 4) return;
  if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h)) return;
  const int w = static_cast<int>(std::lround(r.w)), h = static_cast<int>(std::lround(r.h));
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
  IWICImagingFactory* wic = s_->r_.wic();
  ComPtr<IWICBitmap> src;
  check(wic->CreateBitmapFromMemory(b.width, b.height, GUID_WICPixelFormat32bppPBGRA, b.width * 4,
                                    static_cast<UINT>(b.bgra.size()), const_cast<BYTE*>(b.bgra.data()), &src),
        "CreateBitmapFromMemory");
  ComPtr<IWICBitmapSource> sized = src;
  if (w != b.width || h != b.height) {
    ComPtr<IWICBitmapScaler> scaler;
    check(wic->CreateBitmapScaler(&scaler), "CreateBitmapScaler");
    check(scaler->Initialize(src.Get(), w, h, WICBitmapInterpolationModeFant), "BitmapScaler.Initialize");
    sized = scaler;
  }
  ComPtr<ID2D1Bitmap> bmp;
  check(s_->rt_->CreateBitmapFromWicBitmap(sized.Get(), nullptr, &bmp), "CreateBitmapFromWicBitmap");
  const float x = std::round(r.x), y = std::round(r.y);
  s_->rt_->DrawBitmap(bmp.Get(), D2D1::RectF(x, y, x + static_cast<float>(w), y + static_cast<float>(h)), 1.f,
                      D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

float Canvas::measure(std::wstring_view text, const TextStyle& st) {
  if (!std::isfinite(st.size)) return 0;  // drawn as nothing (text)
  auto l = layout(text, 100000.f, 1000.f, st);
  DWRITE_TEXT_METRICS m{};
  l->GetMetrics(&m);
  return m.widthIncludingTrailingWhitespace;
}

}  // namespace ld
