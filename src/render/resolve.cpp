// src/render/resolve.cpp
#include "render/resolve.h"
#include <algorithm>
#include <cmath>

namespace ld {

double srgb_to_linear(std::uint8_t v) {
  const double c = v / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

std::uint8_t linear_to_srgb(double v) {
  const double c = std::clamp(v, 0.0, 1.0);
  const double s = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
  return static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
}

Rgb gradient_color(const Gradient& g, double value) {
  std::array<GradientStop, 3> s = g.stops;
  std::stable_sort(s.begin(), s.end(), [](const GradientStop& a, const GradientStop& b) { return a.at < b.at; });
  if (!(value > s[0].at)) return s[0].color;  // below the scale, or NaN
  if (value >= s[2].at) return s[2].color;
  const GradientStop& a = value < s[1].at ? s[0] : s[1];
  const GradientStop& b = value < s[1].at ? s[1] : s[2];
  const double t = b.at > a.at ? (value - a.at) / (b.at - a.at) : 1.0;
  auto mix = [t](std::uint8_t x, std::uint8_t y) {
    const double lx = srgb_to_linear(x);
    return linear_to_srgb(lx + (srgb_to_linear(y) - lx) * t);
  };
  return Rgb{mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b)};
}

ResolvedStyle resolve(const ElementSpec& spec, const ElementOverride* over, const ElementInput& in,
                      const StatePalette& p, const RoleLook* role) {
  ResolvedStyle r;
  r.visible = over && over->visible ? *over->visible : spec.visible;
  r.row = over && over->row ? *over->row : spec.row;
  r.align = over && over->align ? *over->align : spec.align;
  const bool weighted = spec.kind == ElementKind::Text && !spec.icon;
  const RoleLook* rl = weighted ? role : nullptr;  // icons and graphs have no role
  if (over && over->size) r.size = static_cast<float>(*over->size);
  else if (rl && rl->size) r.size = static_cast<float>(std::clamp(*rl->size, spec.min_size, spec.max_size));
  else r.size = in.size.value_or(spec.size);
  if (over && over->weight && weighted) r.weight = *over->weight;
  else if (rl && rl->weight) r.weight = *rl->weight;
  else r.weight = spec.weight;
  if (rl) r.font = rl->font;

  ColorMode mode = spec.color_mode;
  Rgb base = in.color.value_or(spec.color);
  const bool chosen = over && over->color;
  if (chosen) {
    mode = over->color->mode;
    if (mode == ColorMode::Fixed) base = over->color->color;
  }
  const bool follows = !spec.keeps_color || (chosen && mode != ColorMode::Fixed);
  r.color = base;
  if (follows && in.state == ElementState::Idle) r.color = p.idle;
  else if (follows && in.state == ElementState::Stale) r.color = p.stale;
  else if (mode == ColorMode::Gradient && in.metric && std::isfinite(*in.metric) && in.gradient) r.color = gradient_color(*in.gradient, *in.metric);
  else if (mode != ColorMode::Fixed && in.state == ElementState::Warning) r.color = p.warning;
  else if (mode != ColorMode::Fixed && in.state == ElementState::Critical) r.color = p.critical;
  return r;
}

}  // namespace ld
