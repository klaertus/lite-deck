#include <doctest/doctest.h>
#include <limits>
#include <shlwapi.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "golden.h"
#include "render/renderer.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "psapi.lib")
using namespace ld;
using Microsoft::WRL::ComPtr;

static Rgb decode_pixel(Renderer& r, const std::vector<uint8_t>& jpeg, int x, int y) {
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(jpeg.data(), static_cast<UINT>(jpeg.size())));
  ComPtr<IWICBitmapDecoder> dec;
  REQUIRE(SUCCEEDED(r.wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec)));
  ComPtr<IWICBitmapFrameDecode> frame;
  REQUIRE(SUCCEEDED(dec->GetFrame(0, &frame)));
  ComPtr<IWICFormatConverter> conv;
  r.wic()->CreateFormatConverter(&conv);
  conv->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
  BYTE px[3];
  WICRect rc{x, y, 1, 1};
  conv->CopyPixels(&rc, 3, 3, px);
  return Rgb{px[2], px[1], px[0]};
}

static bool close_to(Rgb a, Rgb b, int tol = 40) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

TEST_CASE("canvas fills and clips to its region") {
  Renderer r;
  Surface s(r, 100, 50);
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
  }
  {
    Canvas c = s.canvas(RectF{50, 0, 50, 50});
    CHECK(c.width() == 50);
    c.fill_rect(RectF{-50, 0, 200, 50}, {255, 0, 0});  // overflows left: must be clipped
  }
  CHECK(s.pixel(10, 10) == Rgb{0, 0, 0});
  CHECK(s.pixel(75, 25) == Rgb{255, 0, 0});
}

TEST_CASE("text draws visible pixels and measure grows with length") {
  Renderer r;
  Surface s(r, 112, 112);
  TextStyle st;
  st.size = 40;
  st.bold = true;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.text(L"88", RectF{0, 0, 112, 112}, st);
    CHECK(c.measure(L"8888", st) > c.measure(L"88", st));
  }
  int lit = 0;
  for (int y = 0; y < 112; y += 4)
    for (int x = 0; x < 112; x += 4)
      if (s.pixel(x, y).r > 128) ++lit;
  CHECK(lit > 10);
}

TEST_CASE("encode_jpeg produces a baseline JPEG, optionally rotated 180 degrees") {
  Renderer r;
  Surface s(r, 112, 112);
  {
    Canvas c = s.canvas();
    c.fill_rect(RectF{0, 0, 56, 112}, {255, 0, 0});    // left red
    c.fill_rect(RectF{56, 0, 56, 112}, {0, 0, 255});   // right blue
  }
  auto plain = s.encode_jpeg(85, false);
  REQUIRE(plain.size() > 100);
  CHECK(plain[0] == 0xFF); CHECK(plain[1] == 0xD8);
  CHECK(plain[plain.size() - 2] == 0xFF); CHECK(plain.back() == 0xD9);
  CHECK(close_to(decode_pixel(r, plain, 10, 50), {255, 0, 0}));

  auto rotated = s.encode_jpeg(85, true);
  CHECK(close_to(decode_pixel(r, rotated, 10, 50), {0, 0, 255}));
}

TEST_CASE("a surface can be drawn again after encoding") {
  Renderer r;
  Surface s(r, 20, 20);
  { Canvas c = s.canvas(); c.clear({0, 255, 0}); }
  s.encode_jpeg(80, false);
  { Canvas c = s.canvas(); c.clear({255, 255, 255}); }
  CHECK(s.pixel(5, 5) == Rgb{255, 255, 255});
}

TEST_CASE("text format cache snaps sizes and stays bounded") {
  Renderer r;
  TextStyle st;
  for (int i = 0; i < 1000; ++i) {
    st.size = 10.f + static_cast<float>(i) * 0.013f;  // an animated size
    r.text_format(st);
  }
  CHECK(r.cached_formats() <= Renderer::kMaxFormats);
  Renderer r2;
  st.size = 22.1f;
  auto* a = r2.text_format(st);
  st.size = 22.2f;
  CHECK(r2.text_format(st) == a);  // same half-pixel bucket
}

TEST_CASE("color emoji are drawn in color") {
  Renderer r;
  Surface s(r, 112, 112);
  TextStyle st;
  st.size = 60;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.text(L"😀", RectF{0, 0, 112, 112}, st);  // grinning face: yellow
  }
  int yellow = 0;
  for (int y = 0; y < 112; y += 2)
    for (int x = 0; x < 112; x += 2) {
      Rgb p = s.pixel(x, y);
      if (p.r > 180 && p.g > 120 && p.b < 90) ++yellow;
    }
  CHECK(yellow > 50);
}

static std::size_t private_bytes() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return pmc.PrivateUsage;
}

// Regression: D2D's color font path (D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT) took ~10 MB at the first
// emoji and ~100 KB more per new emoji, never released (+20 MB for these 200). Drawn as layers: ~1 MB.
TEST_CASE("drawing many different color emoji keeps private memory flat") {
  Renderer r;
  Surface s(r, 800, 112);
  TextStyle st;
  st.size = 32;
  auto emoji = [](int k) {
    std::wstring t;
    for (int j = 0; j < 4; ++j) {
      char32_t cp = 0x1F300 + static_cast<char32_t>(k * 4 + j) - 0x10000;
      t.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      t.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    }
    return t;
  };
  auto draw = [&](int k) {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.text(emoji(k), RectF{0, 0, 800, 112}, st);
  };
  draw(0);
  s.encode_jpeg(80, true);
  const std::size_t before = private_bytes();
  for (int k = 1; k <= 50; ++k) {
    draw(k);
    s.encode_jpeg(80, true);
  }
  const std::size_t after = private_bytes();
  const double grown_mb = static_cast<double>(after - std::min(before, after)) / (1024.0 * 1024.0);
  MESSAGE("private memory grew by " << grown_mb << " MB for 200 new emoji");
  CHECK(grown_mb < 6.0);
}

TEST_CASE("pixels returns the surface as BGRA rows") {
  Renderer r;
  Surface s(r, 3, 2);
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.fill_rect(RectF{2, 1, 1, 1}, {255, 0, 0});
  }
  auto px = s.pixels();
  REQUIRE(px.size() == 3 * 2 * 4);
  CHECK(px[0] == 0);
  CHECK(px[3] == 255);  // opaque
  const std::size_t i = (1 * 3 + 2) * 4;
  CHECK(px[i + 0] == 0);    // B
  CHECK(px[i + 1] == 0);    // G
  CHECK(px[i + 2] == 255);  // R
  CHECK(px[i + 3] == 255);
}

namespace {
constexpr ElementSpec kValueEl{"value", "Value", 34, 700, 10, 80};
constexpr ElementSpec kIconEl{"icon", "Icon", 26, 400, 10, 80, true};
}  // namespace

TEST_CASE("italic and the family enter the text format cache key; an empty font is Segoe UI") {
  Renderer r;
  TextStyle st;
  st.size = 20;
  IDWriteTextFormat* plain = r.text_format(st, L"Segoe UI", false);
  CHECK(r.text_format(st) == plain);  // TextStyle::font is empty: Segoe UI, upright
  IDWriteTextFormat* italic = r.text_format(st, L"Segoe UI", true);
  CHECK(italic != plain);
  CHECK(italic->GetFontStyle() == DWRITE_FONT_STYLE_ITALIC);
  CHECK(plain->GetFontStyle() == DWRITE_FONT_STYLE_NORMAL);
  CHECK(r.text_format(st, L"Consolas", false) != plain);
  CHECK(r.cached_formats() == 3);
}

TEST_CASE("a canvas with the default look draws exactly what a canvas without one draws") {
  Renderer r;
  Surface a(r, 112, 112), b(r, 112, 112);
  const SlotLook look;
  TextStyle st;
  st.size = 30;
  st.bold = true;
  {
    Canvas c = a.canvas();
    c.clear({0, 0, 0});
    c.text(L"Ab 12", RectF{0, 0, 112, 112}, st);
  }
  {
    Canvas c = b.canvas();
    c.set_look(&look);
    c.clear({0, 0, 0});
    c.text(L"Ab 12", RectF{0, 0, 112, 112}, st);
  }
  CHECK(same_rows(a, b, 0, 112));
}

TEST_CASE("the slot font and italic apply to inherited text only; icons keep Segoe UI upright") {
  Renderer r;
  SlotLook look;
  look.font = L"Segoe UI Symbol";  // has a monochrome sun: an icon must never use it
  look.italic = true;
  auto paint = [&](Surface& s, const SlotLook* l) {
    Canvas c = s.canvas();
    c.set_look(l);
    c.clear({0, 0, 0});
    TextStyle icon = c.style(kIconEl);
    icon.color = Rgb{255, 255, 255};
    c.text(L"☀️", RectF{0, 0, 112, 40}, icon);
    TextStyle text = c.style(kValueEl);
    c.text(L"Wide", RectF{0, 50, 112, 60}, text);
  };
  Surface plain(r, 112, 112), styled(r, 112, 112);
  paint(plain, nullptr);
  paint(styled, &look);
  CHECK(same_rows(plain, styled, 0, 40));         // the icon: identical
  CHECK(saturated(styled, 0, 0, 112, 40) > 20);   // and in colour
  CHECK_FALSE(same_rows(plain, styled, 50, 110)); // the text: another font, slanted
}

TEST_CASE("Canvas::style gives the element's defaults, then the slot's overrides") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  TextStyle st = c.style(kValueEl);
  CHECK(st.size == 34.f);
  CHECK(st.weight == 700);
  CHECK(st.font.empty());                        // inherits the slot font
  CHECK(c.style(kValueEl, 50.f).size == 50.f);   // a default computed by the module
  CHECK(c.style(kIconEl).font == L"Segoe UI");   // icons name their font
  SlotLook look;
  look.elements["value"] = ElementOverride{30, 400};
  look.elements["icon"] = ElementOverride{40, 700};
  c.set_look(&look);
  CHECK(c.style(kValueEl).size == 30.f);
  CHECK(c.style(kValueEl, 50.f).size == 30.f);   // the user's size wins over the computed one
  CHECK(c.style(kValueEl).weight == 400);
  CHECK(c.style(kIconEl).size == 40.f);
  CHECK(c.style(kIconEl).weight == 400);         // icons have no weight
  CHECK(&c.look() == &look);
  c.set_look(nullptr);
  CHECK(c.look().font == L"Segoe UI");
  CHECK_FALSE(c.look().italic);
}

TEST_CASE("text layouts are cached by text and style, not by position or color") {
  Renderer r;
  TextStyle st;
  st.size = 24;
  st.align = HAlign::Left;
  auto a = r.text_layout(L"Hello", st, L"Segoe UI", false, 200, 50);
  auto b = r.text_layout(L"Hello", st, L"Segoe UI", false, 200, 50);
  CHECK(a.Get() == b.Get());
  CHECK(r.cached_layouts() == 1);
  st.color = Rgb{255, 0, 0};  // the color is the brush's, not the layout's
  CHECK(r.text_layout(L"Hello", st, L"Segoe UI", false, 200, 50).Get() == a.Get());
  st.size = 24.2f;             // same half-pixel bucket as 24
  CHECK(r.text_layout(L"Hello", st, L"Segoe UI", false, 200, 50).Get() == a.Get());
  CHECK(r.cached_layouts() == 1);

  auto differs = [&](auto change) {
    TextStyle s = st;
    std::wstring_view font = L"Segoe UI";
    bool italic = false;
    float w = 200, h = 50;
    std::wstring text = L"Hello";
    change(s, font, italic, w, h, text);
    return r.text_layout(text, s, font, italic, w, h).Get() != a.Get();
  };
  CHECK(differs([](TextStyle&, std::wstring_view&, bool&, float&, float&, std::wstring& t) { t = L"Hello!"; }));
  CHECK(differs([](TextStyle& s, std::wstring_view&, bool&, float&, float&, std::wstring&) { s.size = 30; }));
  CHECK(differs([](TextStyle& s, std::wstring_view&, bool&, float&, float&, std::wstring&) { s.bold = true; }));
  CHECK(differs([](TextStyle& s, std::wstring_view&, bool&, float&, float&, std::wstring&) { s.weight = 600; }));
  CHECK(differs([](TextStyle& s, std::wstring_view&, bool&, float&, float&, std::wstring&) { s.align = HAlign::Right; }));
  CHECK(differs([](TextStyle& s, std::wstring_view&, bool&, float&, float&, std::wstring&) { s.valign = VAlign::Top; }));
  CHECK(differs([](TextStyle&, std::wstring_view& f, bool&, float&, float&, std::wstring&) { f = L"Consolas"; }));
  CHECK(differs([](TextStyle&, std::wstring_view&, bool& i, float&, float&, std::wstring&) { i = true; }));
  CHECK(differs([](TextStyle&, std::wstring_view&, bool&, float& w, float&, std::wstring&) { w = 201; }));
  CHECK(differs([](TextStyle&, std::wstring_view&, bool&, float&, float& h, std::wstring&) { h = 51; }));
  CHECK(r.cached_layouts() == 11);
}

TEST_CASE("Canvas text drawn at moving positions reuses one layout and draws what a fresh one draws") {
  Renderer r, fresh;
  Surface a(r, 400, 112), b(fresh, 400, 112);
  TextStyle st;
  st.size = 32;
  st.weight = 600;
  st.align = HAlign::Left;
  const std::wstring t = L"Daft Punk - Harder 🎧 Better";
  for (int i = 0; i < 30; ++i) {
    Canvas c = a.canvas(RectF{0, 0, 300, 112});
    c.clear({0, 0, 0});
    const float w = c.measure(t, st);
    c.text(t, RectF{10.f - static_cast<float>(i) * 2.3f, 20, w + 2, 48}, st);
  }
  CHECK(r.cached_layouts() == 2);  // the measure and the drawn copy
  CHECK(r.layout_hits() == 58);
  {
    Canvas c = b.canvas(RectF{0, 0, 300, 112});
    c.clear({0, 0, 0});
    const float w = c.measure(t, st);
    c.text(t, RectF{10.f - 29 * 2.3f, 20, w + 2, 48}, st);
  }
  CHECK(same_rows(a, b, 0, 112));
}

TEST_CASE("the text layout cache stays bounded and keeps the recently used layouts") {
  Renderer r;
  TextStyle st;
  auto first = r.text_layout(L"keep", st, L"Segoe UI", false, 100, 40);
  for (int i = 0; i < 1000; ++i) {
    r.text_layout(L"text " + std::to_wstring(i), st, L"Segoe UI", false, 100, 40);
    r.text_layout(L"keep", st, L"Segoe UI", false, 100, 40);  // used every time: never evicted
    CHECK(r.cached_layouts() <= Renderer::kMaxLayouts);
  }
  CHECK(r.cached_layouts() == Renderer::kMaxLayouts);
  CHECK(r.text_layout(L"keep", st, L"Segoe UI", false, 100, 40).Get() == first.Get());
  const auto misses = r.layout_misses();
  r.text_layout(L"text 0", st, L"Segoe UI", false, 100, 40);  // evicted long ago
  CHECK(r.layout_misses() == misses + 1);
}

TEST_CASE("a cached layout outlives a cleared text format cache") {
  Renderer r;
  Surface s(r, 200, 112);
  TextStyle st;
  st.size = 30;
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.text(L"Ab", RectF{0, 0, 200, 112}, st);
  }
  const auto before = ink(s);
  TextStyle other;
  for (int i = 0; i < static_cast<int>(Renderer::kMaxFormats) + 5; ++i) {  // forces a format cache clear
    other.size = 8.f + static_cast<float>(i);
    r.text_format(other);
  }
  const auto hits = r.layout_hits();
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.text(L"Ab", RectF{0, 0, 200, 112}, st);  // the cached layout holds its own format
  }
  CHECK(r.layout_hits() == hits + 1);
  const auto after = ink(s);
  CHECK(after.count == before.count);
  CHECK(after.left == before.left);
}

TEST_CASE("triangle geometries are built once per corner set and the cache stays bounded") {
  Renderer r;
  auto* g = r.triangle(PointF{0, 0}, PointF{0, 20}, PointF{17, 10});
  CHECK(r.triangle(PointF{0, 0}, PointF{0, 20}, PointF{17, 10}) == g);
  CHECK(r.triangle(PointF{0, 0}, PointF{0, 21}, PointF{17, 10}) != g);
  for (int i = 0; i < 100; ++i) r.triangle(PointF{0, 0}, PointF{0, static_cast<float>(i)}, PointF{5, 5});
  CHECK(r.cached_triangles() <= Renderer::kMaxTriangles);
  Surface s(r, 40, 40);
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    c.fill_triangle(PointF{5, 5}, PointF{5, 35}, PointF{31, 20}, Rgb{0, 255, 0});
    c.fill_triangle(PointF{5, 5}, PointF{5, 35}, PointF{31, 20}, Rgb{0, 255, 0});  // cached: still fills
  }
  CHECK(s.pixel(12, 20) == Rgb{0, 255, 0});
  CHECK(s.pixel(35, 20) == Rgb{0, 0, 0});
}

TEST_CASE("a role font draws its elements in that font, with the slot's italic; other texts keep the slot font") {
  constexpr ElementSpec role_el{.key = "value", .label = "Value", .size = 34, .weight = 700, .min_size = 10,
                                .max_size = 80, .role = TextRole::Value};
  Renderer r;
  auto paint = [&](Surface& s, const SlotLook& l, const ElementSpec& e) {
    Canvas c = s.canvas();
    c.set_look(&l);
    c.clear({0, 0, 0});
    c.text(L"Wide 42", RectF{0, 20, 112, 60}, c.style(e));
  };
  SlotLook italic;  // Segoe UI, slanted
  italic.italic = true;
  SlotLook roled = italic;  // and a Consolas role
  roled.roles[TextRole::Value].font = L"Consolas";
  SlotLook italic_consolas;  // the slot itself in Consolas, slanted
  italic_consolas.font = L"Consolas";
  italic_consolas.italic = true;
  Surface a(r, 112, 112), b(r, 112, 112), c2(r, 112, 112), d(r, 112, 112);
  paint(a, roled, role_el);
  paint(b, italic_consolas, role_el);
  CHECK(same_rows(a, b, 0, 112));  // the role's font, with the slot's italic
  paint(c2, roled, kValueEl);      // no role: the slot's font
  paint(d, italic, kValueEl);
  CHECK(same_rows(c2, d, 0, 112));
  CHECK_FALSE(same_rows(a, d, 0, 112));
}

TEST_CASE("a negative, NaN or infinite text box never throws; a NaN or infinite one draws nothing") {
  Renderer r;
  Surface s(r, 112, 112);
  TextStyle st;
  st.size = 20;
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  {
    Canvas c = s.canvas();
    c.clear({0, 0, 0});
    CHECK_NOTHROW(c.text(L"x", RectF{0, 0, -5, 20}, st));  // an empty box at the top-left corner
    CHECK_NOTHROW(c.text(L"x", RectF{0, 0, 20, -1}, st));
    CHECK_NOTHROW(c.text(L"x", RectF{40, 40, nan, 20}, st));
    CHECK_NOTHROW(c.text(L"x", RectF{40, 40, 40, nan}, st));
    CHECK_NOTHROW(c.text(L"x", RectF{nan, 40, 40, 20}, st));
    CHECK_NOTHROW(c.text(L"x", RectF{40, 40, inf, 20}, st));
    TextStyle bad = st;
    bad.size = nan;
    CHECK_NOTHROW(c.text(L"x", RectF{40, 40, 40, 20}, bad));
    CHECK(c.measure(L"x", bad) == 0);
    CHECK_NOTHROW(r.text_layout(L"x", st, L"Segoe UI", false, -3, nan));
  }
  for (int y = 0; y < 112; ++y)
    for (int x = 0; x < 112; ++x) {
      if (y < 32 && x < 32) continue;  // the empty boxes' text may overhang there
      REQUIRE(s.pixel(x, y) == Rgb{0, 0, 0});
    }
  {  // the next frame draws as usual
    Canvas c = s.canvas();
    c.text(L"88", RectF{0, 0, 112, 112}, st);
  }
  bool ink = false;
  for (int x = 0; x < 112 && !ink; ++x) ink = s.pixel(x, 56).r > 100;
  CHECK(ink);
}
