// src/modules/weather/normals.cpp
#include "modules/weather/normals.h"
#include <cmath>
#include <format>

namespace ld {
using nlohmann::json;

int day_slot(int month, int day) {
  static constexpr int kStart[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
  if (month < 1 || month > 12 || day < 1 || day > 31) return -1;
  return kStart[month - 1] + day - 1;
}

std::string normals_url(double lat, double lon, int first_year, int last_year) {
  return std::format(
      "https://archive-api.open-meteo.com/v1/archive?latitude={:.4f}&longitude={:.4f}"
      "&start_date={}-01-01&end_date={}-12-31&daily=temperature_2m_mean&timezone=auto",
      lat, lon, first_year, last_year);
}

std::optional<std::vector<std::pair<CivilDate, double>>> parse_daily_means(std::string_view body) {
  const json j = json::parse(body, nullptr, false);
  if (!j.is_object() || !j.contains("daily") || !j["daily"].is_object()) return std::nullopt;
  const json& d = j["daily"];
  if (!d.contains("time") || !d["time"].is_array() || !d.contains("temperature_2m_mean") ||
      !d["temperature_2m_mean"].is_array() || d["time"].size() != d["temperature_2m_mean"].size())
    return std::nullopt;
  std::vector<std::pair<CivilDate, double>> out;
  out.reserve(d["time"].size());
  for (std::size_t i = 0; i < d["time"].size(); ++i) {
    const json& t = d["time"][i];
    const json& v = d["temperature_2m_mean"][i];
    if (!t.is_string() || !v.is_number()) continue;
    const auto date = parse_iso_date(t.get<std::string>());
    if (date) out.emplace_back(*date, v.get<double>());
  }
  if (out.empty()) return std::nullopt;
  return out;
}

std::optional<Normals> compute_normals(const std::vector<std::pair<CivilDate, double>>& days) {
  std::array<double, 366> sum{};
  std::array<int, 366> count{};
  for (const auto& [date, value] : days) {
    const int s = day_slot(date.month, date.day);
    if (s < 0 || !std::isfinite(value)) continue;
    sum[s] += value;
    ++count[s];
  }
  Normals out{};
  for (int d = 0; d < 366; ++d) {
    double total = 0;
    int n = 0;
    for (int k = -kNormalWindow; k <= kNormalWindow; ++k) {
      const int s = ((d + k) % 366 + 366) % 366;
      total += sum[s];
      n += count[s];
    }
    if (n == 0) return std::nullopt;
    out[d] = total / n;
  }
  return out;
}

double normal_for(const Normals& normals, CivilDate date) {
  const int s = day_slot(date.month, date.day);
  return s < 0 ? normals[0] : normals[s];
}

json normals_to_json(const CachedNormals& n) {
  json values = json::array();
  for (double v : n.values) values.push_back(std::round(v * 10.0) / 10.0);
  return json{{"fetched", format_iso_date(n.fetched)}, {"years", n.years}, {"values", std::move(values)}};
}

std::optional<CachedNormals> normals_from_json(const json& j) {
  if (!j.is_object() || !j.contains("fetched") || !j["fetched"].is_string() || !j.contains("values") ||
      !j["values"].is_array() || j["values"].size() != 366)
    return std::nullopt;
  const auto fetched = parse_iso_date(j["fetched"].get<std::string>());
  if (!fetched) return std::nullopt;
  CachedNormals out;
  out.fetched = *fetched;
  if (j.contains("years") && j["years"].is_number_integer()) out.years = j["years"].get<int>();
  for (std::size_t i = 0; i < 366; ++i) {
    if (!j["values"][i].is_number()) return std::nullopt;
    out.values[i] = j["values"][i].get<double>();
  }
  return out;
}

}  // namespace ld
