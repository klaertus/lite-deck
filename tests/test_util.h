#pragma once
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

// Builds a 513-byte input report from a hex prefix such as "00 41 43 4b ...".
inline std::vector<uint8_t> report_from_hex(const std::string& hex) {
  std::vector<uint8_t> r(513, 0);
  std::istringstream in(hex);
  std::string byte;
  size_t i = 0;
  while (in >> byte && i < r.size()) r[i++] = static_cast<uint8_t>(std::stoul(byte, nullptr, 16));
  return r;
}

// Returns the ASCII command tag right after "CRT\0\0" (e.g. "BAT", "STP").
template <class Report>
std::string report_tag(const Report& r) {
  std::string s;
  for (size_t i = 6; i < r.size() && r[i] >= 'A' && r[i] <= 'Z'; ++i) s.push_back(static_cast<char>(r[i]));
  return s;
}
