#include <doctest/doctest.h>
#include "web/auth.h"

TEST_CASE("random_token is 32 lowercase hex characters and differs each time") {
  const std::string a = ld::random_token();
  const std::string b = ld::random_token();
  CHECK(a.size() == 32);
  CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(a != b);
}

TEST_CASE("host_allowed accepts only the loopback names with the exact port") {
  CHECK(ld::host_allowed("127.0.0.1:7878", 7878));
  CHECK(ld::host_allowed("localhost:7878", 7878));
  CHECK(ld::host_allowed("LocalHost:7878", 7878));
  CHECK_FALSE(ld::host_allowed("127.0.0.1:7879", 7878));
  CHECK_FALSE(ld::host_allowed("127.0.0.1:17878", 7878));
  CHECK_FALSE(ld::host_allowed("127.0.0.1", 7878));
  CHECK_FALSE(ld::host_allowed("evil.example:7878", 7878));
  CHECK_FALSE(ld::host_allowed("localhost.evil.example:7878", 7878));
  CHECK_FALSE(ld::host_allowed("x127.0.0.1:7878", 7878));
  CHECK_FALSE(ld::host_allowed("[::1]:7878", 7878));
  CHECK_FALSE(ld::host_allowed("localhost.:7878", 7878));
  CHECK_FALSE(ld::host_allowed("127.0.0.1.:7878", 7878));
  CHECK_FALSE(ld::host_allowed("", 7878));
}

TEST_CASE("cookie_value finds one cookie among others") {
  CHECK(ld::cookie_value("a=1; ld=xyz; b=2", "ld") == "xyz");
  CHECK(ld::cookie_value("ld=xyz", "ld") == "xyz");
  CHECK(ld::cookie_value("old=1;ld = xyz ", "ld") == "xyz");
  CHECK(ld::cookie_value("xld=1; ldx=2", "ld").empty());
  CHECK(ld::cookie_value("ld=first; ld=second", "ld") == "first");
  CHECK(ld::cookie_value("", "ld").empty());
}

TEST_CASE("same_token never accepts empty or different tokens") {
  CHECK(ld::same_token("abc", "abc"));
  CHECK_FALSE(ld::same_token("abc", "abd"));
  CHECK_FALSE(ld::same_token("abc", "abcd"));
  CHECK_FALSE(ld::same_token("", ""));
}

TEST_CASE("config_page_url and is_json_content_type") {
  CHECK(ld::config_page_url(7878, "abc") == "http://127.0.0.1:7878/?t=abc");
  CHECK(ld::is_json_content_type("application/json"));
  CHECK(ld::is_json_content_type("Application/JSON; charset=UTF-8"));
  CHECK_FALSE(ld::is_json_content_type("application/jsonp"));
  CHECK_FALSE(ld::is_json_content_type("text/plain"));
  CHECK_FALSE(ld::is_json_content_type(""));
}
