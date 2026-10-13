// tests/test_key_renderer.cpp: the key engine, the style resolution and the gradients.
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include "golden.h"
#include "modules/common/fit.h"
#include "modules/key_module.h"
#include "modules/module.h"
#include "render/key_renderer.h"
#include "render/look_json.h"
#include "render/resolve.h"

using namespace ld;

namespace {

constexpr Rgb kTeal{0, 224, 160};
constexpr Rgb kGrey{140, 140, 140};
constexpr ElementSpec kLabel{.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                             .row = Row::Top, .align = HAlign::Left, .color = kGrey, .keeps_color = true};
constexpr ElementSpec kTemp{.key = "temp", .label = "Temperature", .size = 17, .weight = 600, .min_size = 8,
                            .max_size = 30, .row = Row::Top, .align = HAlign::Right, .color_mode = ColorMode::State,
                            .color = kTeal, .gradient = true};
constexpr ElementSpec kValue{.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10,
                             .max_size = 80, .row = Row::Middle, .color = kTeal};
constexpr ElementSpec kUnit{.key = "unit", .label = "Unit", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                            .row = Row::Bottom, .color = kGrey, .keeps_color = true};
constexpr ElementSpec kGraph{.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
                             .kind = ElementKind::Graph, .row = Row::Bottom, .color = kTeal};
constexpr ElementSpec kIconSpec{.key = "icon", .label = "Icon", .size = 26, .weight = 400, .min_size = 10,
                                .max_size = 80, .icon = true, .kind = ElementKind::Icon, .row = Row::Top};
constexpr ElementSpec kSpecs[] = {kLabel, kTemp, kValue, kUnit, kGraph, kIconSpec};

const Gradient kScale{{GradientStop{40, Rgb{0x4f, 0xa3, 0xff}}, GradientStop{65, Rgb{255, 255, 255}},
                       GradientStop{90, Rgb{0xff, 0x5a, 0x3c}}}};

const KeyItem* item(const std::vector<KeyItem>& items, const char* key) {
  for (const KeyItem& it : items)
    if (std::string_view(it.spec->key) == key) return &it;
  return nullptr;
}

std::unique_ptr<Surface> paint(Renderer& r, std::span<const KeyElement> content, std::span<const ElementSpec> specs,
                               const SlotLook* look = nullptr) {
  auto s = std::make_unique<Surface>(r, 112, 112);
  {
    Canvas c = s->canvas();
    c.set_look(look);
    c.clear({0, 0, 0});
    render_key(c, content, specs);
  }
  return s;
}

// A key module for the KeyModule test: one value.
class FortyTwo final : public KeyModule {
public:
  FortyTwo() : KeyModule(kSpecs) {}
  void start(ModuleContext&, const nlohmann::json&) override {}
  std::vector<KeyElement> content() override { return {KeyElement{.key = "value", .text = L"42"}}; }
};

}  // namespace

TEST_CASE("gradient_color: stops, clamping, a linear-light midpoint, any stop order") {
  const Gradient g{{GradientStop{0, Rgb{0, 0, 0}}, GradientStop{10, Rgb{255, 255, 255}}, GradientStop{20, Rgb{255, 0, 0}}}};
  CHECK(gradient_color(g, -5) == Rgb{0, 0, 0});        // clamped below
  CHECK(gradient_color(g, 0) == Rgb{0, 0, 0});
  CHECK(gradient_color(g, 5) == Rgb{188, 188, 188});   // half of the light, not half of the sRGB code
  CHECK(gradient_color(g, 10) == Rgb{255, 255, 255});
  CHECK(gradient_color(g, 15) == Rgb{255, 188, 188});
  CHECK(gradient_color(g, 25) == Rgb{255, 0, 0});      // clamped above
  CHECK(gradient_color(g, std::numeric_limits<double>::quiet_NaN()) == Rgb{0, 0, 0});
  const Gradient shuffled{{g.stops[2], g.stops[0], g.stops[1]}};
  CHECK(gradient_color(shuffled, 5) == Rgb{188, 188, 188});
  CHECK(linear_to_srgb(srgb_to_linear(77)) == 77);
}

TEST_CASE("resolve: the module's defaults, then the user's settings") {
  const StatePalette p;
  ElementInput in;
  ResolvedStyle r = resolve(kValue, nullptr, in, p);
  CHECK(r.visible);
  CHECK(r.row == Row::Middle);
  CHECK(r.align == HAlign::Center);
  CHECK(r.size == 36.f);
  CHECK(r.weight == 700);
  CHECK(r.color == kTeal);
  in.size = 27.f;
  in.color = Rgb{1, 2, 3};
  r = resolve(kValue, nullptr, in, p);
  CHECK(r.size == 27.f);           // the module's computed default
  CHECK(r.color == Rgb{1, 2, 3});  // the module's default colour (e.g. per gpu mode)
  ElementOverride o;
  o.size = 50;
  o.weight = 400;
  o.visible = false;
  o.row = Row::Bottom;
  o.align = HAlign::Right;
  o.color = ColorChoice{ColorMode::Fixed, Rgb{9, 9, 9}};
  r = resolve(kValue, &o, in, p);
  CHECK(r.size == 50.f);
  CHECK(r.weight == 400);
  CHECK_FALSE(r.visible);
  CHECK(r.row == Row::Bottom);
  CHECK(r.align == HAlign::Right);
  CHECK(r.color == Rgb{9, 9, 9});
  o.weight = 700;
  CHECK(resolve(kIconSpec, &o, {}, p).weight == 400);  // icons ignore a weight
}

TEST_CASE("resolve: idle and stale win, then the gradient, then warning and critical, then the fixed colour") {
  StatePalette p;
  p.warning = Rgb{1, 0, 0};
  p.critical = Rgb{2, 0, 0};
  p.idle = Rgb{3, 0, 0};
  p.stale = Rgb{4, 0, 0};
  ElementInput in;
  in.state = ElementState::Warning;
  CHECK(resolve(kTemp, nullptr, in, p).color == p.warning);  // kTemp is in State mode
  in.state = ElementState::Critical;
  CHECK(resolve(kTemp, nullptr, in, p).color == p.critical);
  CHECK(resolve(kValue, nullptr, in, p).color == kTeal);  // Fixed mode: no warning colours
  in.metric = 65;
  in.gradient = kScale;
  ElementOverride grad;
  grad.color = ColorChoice{ColorMode::Gradient, {}};
  CHECK(resolve(kTemp, &grad, in, p).color == Rgb{255, 255, 255});  // the gradient beats critical
  in.state = ElementState::Stale;
  CHECK(resolve(kTemp, &grad, in, p).color == p.stale);
  in.state = ElementState::Idle;
  CHECK(resolve(kTemp, &grad, in, p).color == p.idle);
  ElementOverride fixed;
  fixed.color = ColorChoice{ColorMode::Fixed, Rgb{7, 7, 7}};
  in.state = ElementState::Critical;
  CHECK(resolve(kTemp, &fixed, in, p).color == Rgb{7, 7, 7});  // a fixed colour ignores warnings...
  in.state = ElementState::Stale;
  CHECK(resolve(kTemp, &fixed, in, p).color == p.stale);       // ...but a dead value still goes grey
  in.metric.reset();
  in.state = ElementState::Warning;
  CHECK(resolve(kTemp, &grad, in, p).color == p.warning);  // a gradient without a metric: the state
  for (double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity()}) {
    in.metric = bad;  // a non-finite metric falls back like a missing one
    CHECK(resolve(kTemp, &grad, in, p).color == p.warning);
  }
  in.state = ElementState::Normal;
  in.metric = std::numeric_limits<double>::quiet_NaN();
  CHECK(resolve(kTemp, &grad, in, p).color == kTeal);
}

TEST_CASE("resolve: labels and units keep their colour unless the user picks state") {
  const StatePalette p;
  ElementInput in;
  in.state = ElementState::Stale;
  CHECK(resolve(kLabel, nullptr, in, p).color == kGrey);
  ElementOverride st;
  st.color = ColorChoice{ColorMode::State, {}};
  CHECK(resolve(kLabel, &st, in, p).color == p.stale);
}

TEST_CASE("Canvas::resolve reads the slot's overrides and palette") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  SlotLook look;
  look.states.stale = Rgb{5, 6, 7};
  look.elements["value"].size = 44;
  c.set_look(&look);
  ElementInput in;
  in.state = ElementState::Stale;
  const ResolvedStyle rs = c.resolve(kValue, in);
  CHECK(rs.size == 44.f);
  CHECK(rs.color == Rgb{5, 6, 7});
  const TextStyle st = c.style(kValue, rs);
  CHECK(st.size == 44.f);
  CHECK(st.weight == 700);
  CHECK(st.color == Rgb{5, 6, 7});
  CHECK(st.font.empty());
  CHECK(c.style(kIconSpec, c.resolve(kIconSpec)).font == L"Segoe UI");
}

TEST_CASE("one element alone in its row is drawn exactly as fit_text draws it in that row") {
  Renderer r;
  for (const wchar_t* text : {L"12.47", L"123456789"}) {  // fits; shrinks
    const KeyElement content[] = {{.key = "value", .text = text}};
    auto engine = paint(r, content, kSpecs);
    Surface direct(r, 112, 112);
    {
      Canvas c = direct.canvas();
      c.clear({0, 0, 0});
      TextStyle st = c.style(kValue);
      st.color = kTeal;
      fit_text(c, text, kKeyTemplate.middle, st, kValue.min_size);
    }
    CHECK(same_rows(*engine, direct, 0, 112));
  }
  // Top row, left and right alignment, alone: fits; shrinks.
  for (const ElementSpec* spec : {&kLabel, &kTemp}) {
    for (const wchar_t* text : {L"CPU", L"Processor load"}) {
      INFO(spec->key);
      const KeyElement content[] = {{.key = spec->key, .text = text}};
      auto engine = paint(r, content, kSpecs);
      Surface direct(r, 112, 112);
      {
        Canvas c = direct.canvas();
        c.clear({0, 0, 0});
        TextStyle st = c.style(*spec);
        st.color = spec->color;
        st.align = spec->align;
        fit_text(c, text, kKeyTemplate.top, st, spec->min_size);
      }
      CHECK(same_rows(*engine, direct, 0, 112));
    }
  }
}

TEST_CASE("a text in the Line2 row of a two-line key is placed in that row") {
  constexpr ElementSpec line2{.key = "line2", .label = "Line 2", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line2};
  const ElementSpec specs[] = {line2};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "line2", .text = L"12:34"}};
  const auto items = layout_key(c, content, specs);
  REQUIRE(items.size() == 1);
  CHECK(items[0].rect.x == 6.f);
  CHECK(items[0].rect.y == 58.f);
  CHECK(items[0].rect.w == 100.f);
  CHECK(items[0].rect.h == 46.f);
  CHECK(items[0].text.align == HAlign::Center);
  CHECK(items[0].text.valign == VAlign::Center);
}

TEST_CASE("the two lines of a two-line key share one size, unless the user sized them apart") {
  constexpr ElementSpec sym1{.key = "symbol1", .label = "Symbol 1", .size = 17, .weight = 600, .min_size = 9,
                             .max_size = 40, .row = Row::Line1, .align = HAlign::Left};
  constexpr ElementSpec line1{.key = "line1", .label = "Line 1", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line1, .align = HAlign::Right};
  constexpr ElementSpec sym2{.key = "symbol2", .label = "Symbol 2", .size = 17, .weight = 600, .min_size = 9,
                             .max_size = 40, .row = Row::Line2, .align = HAlign::Left};
  constexpr ElementSpec line2{.key = "line2", .label = "Line 2", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line2, .align = HAlign::Right};
  const ElementSpec specs[] = {sym1, line1, sym2, line2};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "symbol1", .text = L"A"},
                                {.key = "line1", .text = L"123 456 789"},
                                {.key = "symbol2", .text = L"B"},
                                {.key = "line2", .text = L"243"}};
  auto items = layout_key(c, content, specs);
  REQUIRE(items.size() == 4);
  CHECK(item(items, "line1")->text.size < 26.f);  // too long: shrunk
  CHECK(item(items, "line2")->text.size == item(items, "line1")->text.size);  // and the other line with it
  CHECK(item(items, "symbol1")->text.size == 17.f);  // the symbols fit: untouched
  CHECK(item(items, "symbol2")->text.size == 17.f);
  CHECK(item(items, "line2")->rect.x + item(items, "line2")->rect.w == 106.f);  // still on the right edge
  SlotLook look;  // the user sizes the second line apart: each keeps its own fit
  look.elements["line2"].size = 30;
  c.set_look(&look);
  items = layout_key(c, content, specs);
  CHECK(item(items, "line2")->text.size == 30.f);
  c.set_look(nullptr);
}

TEST_CASE("a text in the middle row is centred vertically in the row") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "value", .text = L"30.2"}};
  const auto items = layout_key(c, content, kSpecs);
  REQUIRE(items.size() == 1);
  CHECK(items[0].rect.x == 6.f);
  CHECK(items[0].rect.y == 24.f);
  CHECK(items[0].rect.w == 100.f);
  CHECK(items[0].rect.h == 52.f);
  CHECK(items[0].text.align == HAlign::Center);
  CHECK(items[0].text.valign == VAlign::Center);
}

TEST_CASE("label left and temperature right share the top row, each at its own edge") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "label", .text = L"CPU"}, {.key = "temp", .text = L"52 °C"}};
  const auto items = layout_key(c, content, kSpecs);
  const KeyItem* label = item(items, "label");
  const KeyItem* temp = item(items, "temp");
  REQUIRE(label);
  REQUIRE(temp);
  CHECK(label->rect.x == 6.f);
  CHECK(label->text.align == HAlign::Left);
  CHECK(temp->rect.x + temp->rect.w == doctest::Approx(106.f));
  CHECK(temp->text.align == HAlign::Right);
  CHECK(label->rect.y == 4.f);
  CHECK(label->rect.h == 20.f);
  CHECK(temp->text.size == 17.f);  // both fit: nothing shrinks
  CHECK(label->text.size == 17.f);
}

TEST_CASE("two elements with the same alignment sit side by side in declaration order") {
  constexpr ElementSpec a{.key = "a", .label = "A", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                          .row = Row::Top, .align = HAlign::Left};
  constexpr ElementSpec b{.key = "b", .label = "B", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                          .row = Row::Top, .align = HAlign::Left};
  const ElementSpec specs[] = {a, b};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "b", .text = L"BB"}, {.key = "a", .text = L"AA"}};
  auto items = layout_key(c, content, specs);
  const KeyItem* first = item(items, "a");
  const KeyItem* second = item(items, "b");
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->rect.x == 6.f);  // declaration order, not content order
  CHECK(second->rect.x >= first->rect.x + first->rect.w + kElementGap - 0.01f);
  SlotLook look;
  look.elements["a"].visible = false;  // a hidden element frees its place
  c.set_look(&look);
  items = layout_key(c, content, specs);
  REQUIRE(items.size() == 1);
  CHECK(items[0].rect.x == 6.f);
}

TEST_CASE("a centre group of two is centred between the left and right groups, like a lone centre text") {
  constexpr ElementSpec c1{.key = "c1", .label = "C1", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                           .row = Row::Top};
  constexpr ElementSpec c2{.key = "c2", .label = "C2", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                           .row = Row::Top};
  constexpr ElementSpec right{.key = "right", .label = "Right", .size = 17, .weight = 600, .min_size = 8,
                              .max_size = 40, .row = Row::Top, .align = HAlign::Right};
  const ElementSpec specs[] = {c1, c2, right};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "right", .text = L"52°"}, {.key = "c2", .text = L"B"}, {.key = "c1", .text = L"A"}};
  const auto items = layout_key(c, content, specs);
  const KeyItem* a = item(items, "c1");
  const KeyItem* b = item(items, "c2");
  const KeyItem* rt = item(items, "right");
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(rt);
  const float lo = kKeyTemplate.top.x;
  const float hi = rt->rect.x - kElementGap;
  CHECK(rt->rect.x + rt->rect.w == doctest::Approx(106.f));
  CHECK(a->rect.x < b->rect.x);  // declaration order
  CHECK(b->rect.x == doctest::Approx(a->rect.x + a->rect.w + kElementGap));
  CHECK(a->rect.x >= lo - 0.01f);  // inside the space left by the right group
  CHECK(b->rect.x + b->rect.w <= hi + 0.01f);
  CHECK((a->rect.x + b->rect.x + b->rect.w) / 2 == doctest::Approx((lo + hi) / 2));  // centred in lo..hi
  // A lone centre text in the same row is centred in the same space.
  const KeyElement lone[] = {{.key = "right", .text = L"52°"}, {.key = "c1", .text = L"A"}};
  const auto items2 = layout_key(c, lone, specs);
  const KeyItem* a2 = item(items2, "c1");
  REQUIRE(a2);
  CHECK(a2->rect.x == doctest::Approx(lo));
  CHECK(a2->rect.x + a2->rect.w == doctest::Approx(hi));
}

TEST_CASE("a text too long even at its minimum is clipped to its share and never covers its neighbour") {
  Renderer r;
  const KeyElement content[] = {{.key = "label", .text = L"A label far too long for any key, even at eight pixels"},
                                {.key = "temp", .text = L"52 °C"}};
  {
    Surface s(r, 112, 112);
    Canvas c = s.canvas();
    const auto items = layout_key(c, content, kSpecs);
    const KeyItem* label = item(items, "label");
    const KeyItem* temp = item(items, "temp");
    REQUIRE(label);
    REQUIRE(temp);
    CHECK(temp->text.size == 17.f);  // the short neighbour keeps its size
    CHECK(temp->clip.w == 0.f);
    CHECK(label->text.size == 8.f);  // shrunk to its minimum...
    CHECK(label->clip.w > 0.f);      // ...and still too wide: clipped
    CHECK(label->clip.x == 6.f);
    CHECK(label->clip.x + label->clip.w <= temp->rect.x - kElementGap + 0.01f);
  }
  auto img = paint(r, content, kSpecs);
  CHECK(ink(*img, 0, 0, 6, 112).count == 0);  // nothing in the side margins
  CHECK(ink(*img, 106, 0, 112, 112).count == 0);
}

TEST_CASE("a graph takes its whole row; the newest point sits on the right edge") {
  Renderer r;
  const KeyElement content[] = {{.key = "graph", .points = {0.f, 1.f}, .capacity = 60}};
  {
    Surface s(r, 112, 112);
    Canvas c = s.canvas();
    const auto items = layout_key(c, content, kSpecs);
    REQUIRE(items.size() == 1);
    CHECK(items[0].rect.x == kKeyTemplate.bottom.x);
    CHECK(items[0].rect.y == kKeyTemplate.bottom.y);
    CHECK(items[0].rect.w == kKeyTemplate.bottom.w);
    CHECK(items[0].rect.h == kKeyTemplate.bottom.h);
  }
  auto img = paint(r, content, kSpecs);
  CHECK(ink(*img, 0, 0, 98, 112).count == 0);  // two points of 60: only the last 2 px
  CHECK(ink(*img, 100, 76, 108, 109).count > 0);
}

TEST_CASE("a graph whose range is empty (hi <= lo) is not drawn, like the plan-4 sparkline") {
  const std::deque<float> values{1.f, 2.f, 3.f};
  CHECK(graph_points(values, 5.f, 5.f).empty());
  CHECK(graph_points(values, 6.f, 5.f).empty());
  CHECK(graph_points(values, 0.f, std::numeric_limits<float>::quiet_NaN()).empty());
  CHECK(graph_points(values, 0.f, 4.f).size() == 3);
  Renderer r;
  const KeyElement content[] = {{.key = "graph", .points = graph_points(values, 5.f, 5.f), .capacity = 60}};
  CHECK(ink(*paint(r, content, kSpecs)).count == 0);
}

TEST_CASE("the longest strings fit their row, at their default size where expected") {
  constexpr ElementSpec weekday{.key = "weekday", .label = "Weekday", .size = 17, .weight = 600, .min_size = 8,
                                .max_size = 40, .row = Row::Top};
  constexpr ElementSpec percent{.key = "percent", .label = "Percent", .size = 36, .weight = 700, .min_size = 10,
                                .max_size = 80};
  constexpr ElementSpec time{.key = "time", .label = "Time", .size = 28, .weight = 700, .min_size = 10, .max_size = 60};
  constexpr ElementSpec total{.key = "total", .label = "Total", .size = 17, .weight = 600, .min_size = 8,
                              .max_size = 40, .row = Row::Bottom};
  constexpr ElementSpec symbol{.key = "symbol", .label = "Symbol", .size = 17, .weight = 600, .min_size = 8,
                               .max_size = 40, .row = Row::Line1, .align = HAlign::Left};
  constexpr ElementSpec price{.key = "price", .label = "Price", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line1, .align = HAlign::Right};
  const ElementSpec specs[] = {weekday, percent, time, total, symbol, price};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  auto check = [&](std::span<const KeyElement> content, const char* key, bool keeps_size) {
    INFO(key);
    const auto items = layout_key(c, content, specs);
    const KeyItem* it = item(items, key);
    REQUIRE(it);
    CHECK(it->clip.w == 0.f);
    CHECK(c.measure(content[it->index].text, it->text) <= 100.f);
    if (keeps_size) CHECK(it->text.size == it->spec->size);
  };
  const KeyElement wd[] = {{.key = "weekday", .text = L"Wednesday"}};
  const KeyElement pc[] = {{.key = "percent", .text = L"100%"}};
  const KeyElement tm[] = {{.key = "time", .text = L"23:59:59"}};
  const KeyElement tt[] = {{.key = "total", .text = L"/ 128 GB"}};
  const KeyElement pr[] = {{.key = "symbol", .text = L"₿"}, {.key = "price", .text = L"99 999"}};
  check(wd, "weekday", true);
  check(pc, "percent", true);
  check(tm, "time", false);  // may shrink a little: the fit is the safety net
  check(tt, "total", true);
  check(pr, "price", true);
}

TEST_CASE("a key with every element hidden draws nothing and does not fail") {
  Renderer r;
  SlotLook look;
  for (const ElementSpec& e : kSpecs) look.elements[e.key].visible = false;
  const KeyElement content[] = {{.key = "label", .text = L"CPU"}, {.key = "value", .text = L"42%"},
                                {.key = "graph", .points = {0.2f, 0.4f, 0.6f}}};
  auto img = paint(r, content, kSpecs, &look);
  CHECK(ink(*img).count == 0);
  look.elements["value"].visible.reset();  // shown again
  CHECK(ink(*paint(r, content, kSpecs, &look)).count > 0);
}

TEST_CASE("empty texts, graphs of fewer than two points and undeclared keys are skipped") {
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "value", .text = L""}, {.key = "graph", .points = {0.5f}},
                                {.key = "nope", .text = L"X"}};
  CHECK(layout_key(c, content, kSpecs).empty());
}

TEST_CASE("a KeyModule draws its content through render_key") {
  Renderer r;
  FortyTwo m;
  auto via_module = draw(m, r);
  const KeyElement content[] = {{.key = "value", .text = L"42"}};
  auto direct = paint(r, content, kSpecs);
  CHECK(same_rows(*via_module, *direct, 0, 112));
  CHECK(ink(*via_module).count > 0);
}

TEST_CASE("role names: six roles, each with its config name; None has none") {
  CHECK(std::string(role_name(TextRole::Label)) == "label");
  CHECK(std::string(role_name(TextRole::LongValue)) == "long_value");
  CHECK(std::string(role_name(TextRole::TwoLine)) == "two_line");
  CHECK(std::string(role_name(TextRole::None)).empty());
  CHECK(kTextRoles.size() == 6);
  for (TextRole role : kTextRoles) {
    INFO(role_name(role));
    CHECK(role_from_name(role_name(role)) == role);
  }
  CHECK_FALSE(role_from_name(""));
  CHECK_FALSE(role_from_name("none"));
  CHECK_FALSE(role_from_name("Value"));
}

TEST_CASE("resolve: the element's setting, then its role, then the module's size, then its declaration") {
  constexpr ElementSpec value{.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10,
                              .max_size = 80, .role = TextRole::Value};
  const StatePalette p;
  const ElementInput none;
  ElementInput computed;
  computed.size = 30.f;
  RoleLook role;
  role.size = 50;
  role.weight = 400;
  role.font = L"Consolas";
  CHECK(resolve(value, nullptr, none, p).size == 36.f);
  CHECK(resolve(value, nullptr, computed, p).size == 30.f);
  CHECK(resolve(value, nullptr, computed, p, &role).size == 50.f);  // the role beats the module's size
  CHECK(resolve(value, nullptr, none, p, &role).weight == 400);
  CHECK(resolve(value, nullptr, none, p, &role).font == L"Consolas");
  ElementOverride over;
  over.size = 44;
  over.weight = 600;
  const ResolvedStyle own = resolve(value, &over, none, p, &role);
  CHECK(own.size == 44.f);  // the element's own setting wins
  CHECK(own.weight == 600);
  CHECK(own.font == L"Consolas");  // no font per element: the role's stays
  RoleLook weight_only;
  weight_only.weight = 600;
  const ResolvedStyle w = resolve(value, nullptr, none, p, &weight_only);
  CHECK(w.size == 36.f);  // what the role leaves unset keeps the default
  CHECK(w.weight == 600);
  CHECK(w.font.empty());  // "" = the slot's font
  CHECK(resolve(value, nullptr, none, p).font.empty());
}

TEST_CASE("resolve: a role size is kept within the element's bounds; icons and graphs ignore roles") {
  constexpr ElementSpec dot{.key = "online", .label = "Dot", .size = 10, .weight = 400, .min_size = 8, .max_size = 20,
                            .role = TextRole::Symbol};
  const StatePalette p;
  RoleLook big;
  big.size = 60;
  big.weight = 700;
  big.font = L"Consolas";
  CHECK(resolve(dot, nullptr, {}, p, &big).size == 20.f);
  RoleLook tiny;
  tiny.size = 8;
  CHECK(resolve(kValue, nullptr, {}, p, &tiny).size == 10.f);  // kValue: at least 10 px
  const ResolvedStyle icon = resolve(kIconSpec, nullptr, {}, p, &big);
  CHECK(icon.size == 26.f);
  CHECK(icon.weight == 400);
  CHECK(icon.font.empty());
  CHECK(resolve(kGraph, nullptr, {}, p, &big).size == 8.f);
}

TEST_CASE("Canvas::resolve applies the slot's role to the elements of that role only") {
  constexpr ElementSpec value{.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10,
                              .max_size = 80, .role = TextRole::Value};
  constexpr ElementSpec time{.key = "time", .label = "Time", .size = 28, .weight = 700, .min_size = 10,
                             .max_size = 60, .role = TextRole::LongValue};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  SlotLook look;
  look.roles[TextRole::Value] = RoleLook{50, std::nullopt, L"Consolas"};
  c.set_look(&look);
  const ResolvedStyle v = c.resolve(value);
  CHECK(v.size == 50.f);
  CHECK(v.font == L"Consolas");
  const TextStyle st = c.style(value, v);
  CHECK(st.font == L"Consolas");
  CHECK(st.slot_italic);
  CHECK(c.style(value).size == 50.f);  // Canvas::style(spec) takes the same path
  CHECK(c.resolve(time).size == 28.f);
  CHECK(c.style(time, c.resolve(time)).font.empty());
  CHECK(c.resolve(kValue).size == 36.f);  // kValue has no role
  c.set_look(nullptr);
}

TEST_CASE("a two-line role size reaches both lines, which still share one fitted size") {
  constexpr ElementSpec line1{.key = "line1", .label = "Line 1", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line1, .align = HAlign::Right, .role = TextRole::TwoLine};
  constexpr ElementSpec line2{.key = "line2", .label = "Line 2", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line2, .align = HAlign::Right, .role = TextRole::TwoLine};
  const ElementSpec specs[] = {line1, line2};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  SlotLook look;
  look.roles[TextRole::TwoLine].size = 34;
  c.set_look(&look);
  const KeyElement content[] = {{.key = "line1", .text = L"98 512"}, {.key = "line2", .text = L"2 431"}};
  auto items = layout_key(c, content, specs);
  REQUIRE(items.size() == 2);
  CHECK(item(items, "line1")->style.size == 34.f);
  CHECK(item(items, "line2")->style.size == 34.f);
  CHECK(item(items, "line1")->text.size <= 34.f);
  CHECK(item(items, "line2")->text.size == item(items, "line1")->text.size);  // one size for both lines
  look.elements["line2"].size = 20;  // the element's own size still sets it apart
  items = layout_key(c, content, specs);
  CHECK(item(items, "line1")->style.size == 34.f);
  CHECK(item(items, "line2")->text.size == 20.f);
  c.set_look(nullptr);
}

TEST_CASE("crypto_gold with its Bitcoin symbol hidden: the two prices still keep one size") {
  const std::vector<ElementSpec>& specs = find_module("crypto_gold")->elements;
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  SlotLook look;
  look.elements["symbol1"].visible = false;
  c.set_look(&look);
  // Long prices: the gold one, beside its symbol, fits smaller than the Bitcoin one, alone in its row.
  const KeyElement content[] = {{.key = "symbol1", .text = L"₿"},
                                {.key = "line1", .text = L"1 098 512"},
                                {.key = "symbol2", .text = L"Au"},
                                {.key = "line2", .text = L"1 098 512"}};
  auto items = layout_key(c, content, specs);
  REQUIRE(items.size() == 3);
  CHECK(item(items, "symbol1") == nullptr);
  CHECK(item(items, "line2")->text.size < 26.f);
  CHECK(item(items, "line1")->text.size == item(items, "line2")->text.size);  // price with price
  CHECK(item(items, "symbol2")->text.size == 17.f);  // never paired with a price
  CHECK(item(items, "line1")->rect.x + item(items, "line1")->rect.w == doctest::Approx(106.f));
  c.set_look(nullptr);
}

TEST_CASE("a shared two-line size is applied before placement: a group keeps its gaps and its edge") {
  constexpr ElementSpec sym1{.key = "symbol1", .label = "Symbol 1", .size = 17, .weight = 600, .min_size = 9,
                             .max_size = 40, .row = Row::Line1, .align = HAlign::Right, .role = TextRole::Symbol};
  constexpr ElementSpec line1{.key = "line1", .label = "Line 1", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line1, .align = HAlign::Right, .role = TextRole::TwoLine};
  constexpr ElementSpec line2{.key = "line2", .label = "Line 2", .size = 26, .weight = 700, .min_size = 9,
                              .max_size = 40, .row = Row::Line2, .align = HAlign::Right, .role = TextRole::TwoLine};
  const ElementSpec specs[] = {sym1, line1, line2};
  Renderer r;
  Surface s(r, 112, 112);
  Canvas c = s.canvas();
  const KeyElement content[] = {{.key = "symbol1", .text = L"A"},
                                {.key = "line1", .text = L"243"},
                                {.key = "line2", .text = L"123 456 789"}};
  auto items = layout_key(c, content, specs);
  REQUIRE(items.size() == 3);
  const KeyItem& a = *item(items, "symbol1");
  const KeyItem& b = *item(items, "line1");
  REQUIRE(b.text.size < 26.f);  // shrunk by the other line, not by its own fit
  CHECK(b.text.size == item(items, "line2")->text.size);
  CHECK(b.rect.w == doctest::Approx(c.measure(L"243", b.text)));  // its box is its final width
  CHECK(b.rect.x + b.rect.w == doctest::Approx(106.f));            // the right group still reaches the edge
  CHECK(b.rect.x - (a.rect.x + a.rect.w) == doctest::Approx(kElementGap));
}

TEST_CASE("key_template: 112 x 112 is kKeyTemplate exactly; other sizes scale by height") {
  auto same = [](RectF a, RectF b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
  const KeyTemplate t = key_template(112, 112);
  CHECK(same(t.top, kKeyTemplate.top));
  CHECK(same(t.middle, kKeyTemplate.middle));
  CHECK(same(t.bottom, kKeyTemplate.bottom));
  CHECK(same(t.line1, kKeyTemplate.line1));
  CHECK(same(t.line2, kKeyTemplate.line2));
  CHECK(t.scale == 1.f);
  const KeyTemplate s = key_template(96, 96);
  CHECK(s.scale == doctest::Approx(96.0 / 112.0));
  CHECK(s.middle.x == doctest::Approx(6 * 96.0 / 112.0));
  CHECK(s.middle.y == doctest::Approx(24 * 96.0 / 112.0));
  CHECK(s.middle.w == doctest::Approx(96 - 12 * 96.0 / 112.0));
  CHECK(s.middle.h == doctest::Approx(52 * 96.0 / 112.0));
  const KeyTemplate wide = key_template(120, 100);  // not square: the height scales, the width lays out
  CHECK(wide.scale == doctest::Approx(100.0 / 112.0));
  CHECK(wide.bottom.y + wide.bottom.h == doctest::Approx(108 * 100.0 / 112.0));
  CHECK(wide.top.w == doctest::Approx(120 - 12 * 100.0 / 112.0));
}

TEST_CASE("a key drawn at 96 px scales its text sizes and keeps to the scaled rows") {
  Renderer r;
  constexpr ElementSpec kValue96{.key = "value", .label = "Value", .size = 36, .weight = 700, .min_size = 10,
                                 .max_size = 80, .role = TextRole::Value};
  const std::array specs{kValue96};
  const std::vector<KeyElement> content{KeyElement{.key = "value", .text = L"42"}};
  float size112 = 0, size96 = 0;
  RectF rect96;
  Surface s112(r, 112, 112), s96(r, 96, 96);
  {
    Canvas c = s112.canvas();
    const auto items = layout_key(c, content, specs);
    REQUIRE(items.size() == 1);
    size112 = items[0].text.size;
  }
  {
    Canvas c = s96.canvas();
    const auto items = layout_key(c, content, specs);
    REQUIRE(items.size() == 1);
    size96 = items[0].text.size;
    rect96 = items[0].rect;
  }
  CHECK(size112 == 36.f);
  CHECK(size96 == doctest::Approx(36 * 96.0 / 112.0));
  const KeyTemplate t = key_template(96, 96);
  CHECK(rect96.y == doctest::Approx(t.middle.y));
  CHECK(rect96.h == doctest::Approx(t.middle.h));
}

TEST_CASE("at 96 px the gap between two texts and the minimum size of a text follow the scale") {
  constexpr float k96 = 96.f / 112.f;
  Renderer r;
  {  // two texts side by side: the gap is kElementGap at this scale
    constexpr ElementSpec a{.key = "a", .label = "A", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                            .row = Row::Top, .align = HAlign::Left};
    constexpr ElementSpec b{.key = "b", .label = "B", .size = 17, .weight = 600, .min_size = 8, .max_size = 40,
                            .row = Row::Top, .align = HAlign::Left};
    const ElementSpec specs[] = {a, b};
    Surface s(r, 96, 96);
    Canvas c = s.canvas();
    const KeyElement content[] = {{.key = "a", .text = L"AA"}, {.key = "b", .text = L"BB"}};
    const auto items = layout_key(c, content, specs);
    const KeyItem* first = item(items, "a");
    const KeyItem* second = item(items, "b");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->rect.x == doctest::Approx(6 * k96));
    CHECK(second->rect.x - (first->rect.x + first->rect.w) == doctest::Approx(kElementGap * k96));
  }
  {  // a text too long for its share shrinks to its minimum at this scale, then is clipped
    Surface s(r, 96, 96);
    Canvas c = s.canvas();
    const KeyElement content[] = {{.key = "label", .text = L"A label far too long for any key, even at eight pixels"},
                                  {.key = "temp", .text = L"52 °C"}};
    const auto items = layout_key(c, content, kSpecs);
    const KeyItem* label = item(items, "label");
    const KeyItem* temp = item(items, "temp");
    REQUIRE(label);
    REQUIRE(temp);
    CHECK(label->text.size == static_cast<float>(std::lround(8 * k96)));  // 7, not 8
    CHECK(temp->text.size == doctest::Approx(17 * k96));
    CHECK(label->clip.w > 0.f);
    CHECK(label->clip.x + label->clip.w <= temp->rect.x - kElementGap * k96 + 0.01f);
  }
}

TEST_CASE("key_template: a key narrower than its margins gets rows of zero width, never negative") {
  const KeyTemplate t = key_template(40, 500);  // margins 2 x 6 x 500/112 = 53.6 px > 40 px
  for (const RectF& row : {t.top, t.middle, t.bottom, t.line1, t.line2}) CHECK(row.w == 0.f);
  CHECK(key_template(112, 112).top.w == 100.f);  // unchanged where the key is wide enough
}
