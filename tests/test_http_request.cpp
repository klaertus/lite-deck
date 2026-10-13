// tests/test_http_request.cpp: the request options a module may add to http_get (one header, plain
// http, the isolated lane), and the url form written to the log.
#include <doctest/doctest.h>
#include "core/http.h"
#include "fake_context.h"
#include "modules/schema.h"

TEST_CASE("HttpRequest: a url alone is https only, without a header, on the shared lane") {
  const ld::HttpRequest r{.url = "https://a.example/x?q=1"};
  CHECK_FALSE(r.header);
  CHECK_FALSE(r.allow_plain_http);
  CHECK(r.lane == ld::HttpLane::Shared);
}

TEST_CASE("valid_http_header refuses names outside the token characters and values with line breaks") {
  CHECK(ld::valid_http_header("Authorization", "Bearer x"));
  CHECK(ld::valid_http_header("X-Api-Key", ""));
  CHECK_FALSE(ld::valid_http_header("", "x"));
  CHECK_FALSE(ld::valid_http_header("Bad Name", "x"));
  CHECK_FALSE(ld::valid_http_header("X:Y", "x"));
  CHECK_FALSE(ld::valid_http_header("X", "a\r\nInjected: 1"));
  CHECK_FALSE(ld::valid_http_header("X", "a\nb"));
}

TEST_CASE("url_for_log keeps the scheme, host and path, never the query, the fragment or credentials") {
  CHECK(ld::url_for_log("https://api.example.com/v1/x?token=secret#frag") == "https://api.example.com/v1/x");
  CHECK(ld::url_for_log("http://user:pass@ha.local:8123/api/states") == "http://ha.local:8123/api/states");
  CHECK(ld::url_for_log("https://host") == "https://host");
  CHECK(ld::url_for_log("not a url?x=1") == "not a url");
}

TEST_CASE("ModuleContext::http_get hands the whole request to the context; a url is a plain request") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::ModuleContext& c = ctx;
  int status = 0;
  c.http_get({.url = "http://h/p",
              .header = std::pair<std::string, std::string>{"X-Key", "v"},
              .allow_plain_http = true,
              .lane = ld::HttpLane::Isolated},
             [&](const ld::HttpResponse& r) { status = r.status; });
  c.http_get("https://h/q", [](const ld::HttpResponse&) {});
  REQUIRE(ctx.sent.size() == 2);
  const ld::HttpRequest& r = ctx.sent[0];
  CHECK(r.url == "http://h/p");
  REQUIRE(r.header);
  CHECK(r.header->first == "X-Key");
  CHECK(r.header->second == "v");
  CHECK(r.allow_plain_http);
  CHECK(r.lane == ld::HttpLane::Isolated);
  CHECK(ctx.sent[1].url == "https://h/q");
  CHECK_FALSE(ctx.sent[1].header);
  CHECK_FALSE(ctx.sent[1].allow_plain_http);
  CHECK(ctx.sent[1].lane == ld::HttpLane::Shared);
  ctx.respond(0, ld::HttpResponse{204, "", ""});
  CHECK(status == 204);
}

TEST_CASE("schema: a secret is a string the page masks") {
  const nlohmann::json schema = nlohmann::json::parse(
      R"({"fields":[{"key":"token","type":"secret","default":"","label":"Token"}]})");
  nlohmann::json out;
  CHECK_FALSE(ld::apply_schema(schema, nlohmann::json::object(), out));
  CHECK(out["token"] == "");
  CHECK_FALSE(ld::apply_schema(schema, {{"token", "abc"}}, out));
  CHECK(out["token"] == "abc");
  const auto e = ld::apply_schema(schema, {{"token", 12}}, out);
  REQUIRE(e);
  CHECK(e->find("token") != std::string::npos);
  CHECK(e->find("12") == std::string::npos);  // the message never repeats a secret
}

TEST_CASE("http_get_blocking refuses an http url unless the request allows it, and a bad header") {
  // Covered without network: the refusals happen before any connection.
  CHECK(ld::http_get_blocking("http://127.0.0.1:9/").error == "https required");
  const ld::HttpResponse bad = ld::http_get_blocking(
      {.url = "https://127.0.0.1:9/", .header = std::pair<std::string, std::string>{"X", "a\r\nB: c"}});
  CHECK(bad.error == "invalid header");
}
