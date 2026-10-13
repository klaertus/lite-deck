// src/web/auth.cpp
#include "web/auth.h"
#include <windows.h>
#include <bcrypt.h>
#include <cctype>
#include <format>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace ld {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

std::string random_token() {
  unsigned char bytes[16];
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    throw std::runtime_error("BCryptGenRandom failed");
  std::string out;
  for (unsigned char b : bytes) out += std::format("{:02x}", b);
  return out;
}

bool host_allowed(std::string_view host, int port) {
  const std::string suffix = ":" + std::to_string(port);
  if (host.size() <= suffix.size() || host.substr(host.size() - suffix.size()) != suffix) return false;
  const std::string_view name = host.substr(0, host.size() - suffix.size());
  return name == "127.0.0.1" || iequals(name, "localhost");
}

std::string cookie_value(std::string_view header, std::string_view name) {
  while (!header.empty()) {
    const auto semi = header.find(';');
    const std::string_view part = trim(header.substr(0, semi));
    header = semi == std::string_view::npos ? std::string_view{} : header.substr(semi + 1);
    const auto eq = part.find('=');
    if (eq != std::string_view::npos && trim(part.substr(0, eq)) == name) return std::string(trim(part.substr(eq + 1)));
  }
  return {};
}

bool same_token(std::string_view a, std::string_view b) {
  if (a.empty() || a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

std::string config_page_url(int port, std::string_view token) {
  return std::format("http://127.0.0.1:{}/?t={}", port, token);
}

bool is_json_content_type(std::string_view value) {
  return iequals(trim(value.substr(0, value.find(';'))), "application/json");
}

}  // namespace ld
