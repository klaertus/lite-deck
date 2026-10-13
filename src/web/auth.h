// src/web/auth.h
#pragma once
#include <string>
#include <string_view>

namespace ld {

std::string random_token();  // 128 bits from BCryptGenRandom, as 32 lowercase hex characters
// Host header check against DNS rebinding: "127.0.0.1:<port>" or "localhost:<port>" only.
bool host_allowed(std::string_view host, int port);
std::string cookie_value(std::string_view cookie_header, std::string_view name);  // "" if absent
bool same_token(std::string_view a, std::string_view b);  // constant time; false if empty
std::string config_page_url(int port, std::string_view token);  // http://127.0.0.1:<port>/?t=<token>
bool is_json_content_type(std::string_view value);  // application/json, parameters allowed

}  // namespace ld
