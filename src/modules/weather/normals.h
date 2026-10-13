// src/modules/weather/normals.h: seasonal normals from the Open-Meteo archive (ERA5).
#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "modules/sun/sun_calc.h"

namespace ld {

// One normal per day of a leap year: 0 = 1 January, 59 = 29 February, 365 = 31 December.
using Normals = std::array<double, 366>;
inline constexpr int kNormalYears = 30;     // the last 30 complete years
inline constexpr int kMinNormalYears = 10;  // fewer when the answer is above the HTTP cap
inline constexpr int kNormalWindow = 7;     // ± days around each day
inline constexpr int kNormalsMaxAgeDays = 30;

int day_slot(int month, int day);
std::string normals_url(double lat, double lon, int first_year, int last_year);
// {"daily": {"time": [...], "temperature_2m_mean": [...]}}: (date, mean) pairs; null means are
// skipped. nullopt if the answer has not that shape, or holds no value.
std::optional<std::vector<std::pair<CivilDate, double>>> parse_daily_means(std::string_view body);
// The normal of each slot: the mean of all values whose slot is within ±kNormalWindow (wrapping at
// the end of the year). nullopt if one slot has no value in its window.
std::optional<Normals> compute_normals(const std::vector<std::pair<CivilDate, double>>& days);
double normal_for(const Normals& normals, CivilDate date);

struct CachedNormals {
  Normals values{};
  CivilDate fetched;
  int years = kNormalYears;
};
// state.json: {"fetched": "2026-10-12", "years": 30, "values": [366 numbers rounded to 0.1]}.
nlohmann::json normals_to_json(const CachedNormals& normals);
std::optional<CachedNormals> normals_from_json(const nlohmann::json& j);

}  // namespace ld
