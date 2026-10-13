// src/modules/weather/weather.h
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "modules/module.h"

namespace ld {
struct GeoPoint {
  double lat = 0, lon = 0;
  // IANA name from the geocoder ("Europe/Brussels"); may be empty. Diagnostics only (logged, saved):
  // times and dates are always computed in the PC's time zone.
  std::string timezone;
};
// A geocoding answer: a place, a valid answer without a match (the city does not exist), or a body
// that does not read as a geocoding answer (captive portal, error page): a transient failure, retried.
enum class GeocodeStatus { Found, NoMatch, Unreadable };
struct GeocodeResult {
  GeocodeStatus status = GeocodeStatus::Unreadable;
  std::optional<GeoPoint> point;  // set when Found
};
struct Weather {
  double temp = 0;
  int code = 0;
  double tmin = 0, tmax = 0;
};
// The temperature colours: its gap to the seasonal normal (default), or the temperature
// itself (gradient_basis = absolute, and the fallback while no normal is known).
inline constexpr Gradient kWeatherAnomalyGradient{{GradientStop{-8, Rgb{0x4f, 0xa3, 0xff}},
                                                   GradientStop{0, Rgb{0xff, 0xff, 0xff}},
                                                   GradientStop{8, Rgb{0xff, 0x5a, 0x3c}}}};
inline constexpr Gradient kWeatherAbsoluteGradient{{GradientStop{0, Rgb{0x4f, 0xa3, 0xff}},
                                                    GradientStop{15, Rgb{0x4c, 0xd9, 0x64}},
                                                    GradientStop{30, Rgb{0xff, 0x5a, 0x3c}}}};
inline constexpr Duration kWeatherPeriod{15 * 60 * 1000};
GeocodeResult read_geocode(std::string_view body);
// The place when found; nullopt for no match and for an unreadable body alike.
std::optional<GeoPoint> parse_geocode(std::string_view body);
std::optional<Weather> parse_forecast(std::string_view body);
std::wstring weather_icon(int wmo_code);
std::string geocode_url(const std::string& city);
std::string forecast_url(GeoPoint p);
ModuleInfo weather_module_info();
}  // namespace ld
