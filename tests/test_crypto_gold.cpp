#include <doctest/doctest.h>
#include "fake_context.h"
#include "golden.h"
#include "module_fixtures.h"
#include "modules/crypto_gold/crypto_gold.h"
#include "modules/schema.h"

static const char* kBody = R"({"bitcoin":{"eur":98512.4},"tether-gold":{"eur":2431.7}})";

static std::unique_ptr<ld::Module> start(FakeContext& ctx) {
  auto info = ld::crypto_gold_module_info();
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(info.settings_schema, nlohmann::json::object(), s));
  auto m = info.create();
  m->start(ctx, s);
  return m;
}

TEST_CASE("parse_prices") {
  auto p = ld::parse_prices(kBody);
  REQUIRE(p);
  CHECK(p->btc == doctest::Approx(98512.4));
  CHECK(p->gold == doctest::Approx(2431.7));
  CHECK_FALSE(ld::parse_prices(R"({"bitcoin":{"eur":1}})"));
  CHECK_FALSE(ld::parse_prices("<html>"));
}

TEST_CASE("crypto_gold fetches at start, every 5 minutes and on tap without overlapping") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  REQUIRE(ctx.requests.size() == 1);
  CHECK(ctx.requests[0].first == ld::kPricesUrl);
  m->on_event({ld::EventKind::Tap});
  CHECK(ctx.requests.size() == 1);  // still in flight
  ctx.respond(0, {200, kBody, ""});
  m->on_event({ld::EventKind::Tap});
  CHECK(ctx.requests.size() == 2);
  ctx.respond(1, {200, kBody, ""});
  loop.advance(ld::kPricesPeriod);
  CHECK(ctx.requests.size() == 3);
}

TEST_CASE("crypto_gold keeps the last value greyed on error") {
  FakeLoop loop;
  FakeContext ctx(loop);
  ld::Renderer r;
  auto m = start(ctx);
  ctx.respond(0, {200, kBody, ""});
  CHECK(saturated(*draw(*m, r), 40, 0, 112, 112) > 20);  // the orange numbers
  m->on_event({ld::EventKind::Tap});
  ctx.respond(1, {0, "", "timeout"});
  auto stale = draw(*m, r);
  CHECK(saturated(*stale, 0, 0, 112, 112) == 0);  // all grey
  CHECK(ink(*stale).count > 0);
}

TEST_CASE("crypto_gold invalidates only on visible change") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  ctx.respond(0, {200, kBody, ""});
  int n = ctx.invalidations;
  m->on_event({ld::EventKind::Tap});
  ctx.respond(1, {200, kBody, ""});
  CHECK(ctx.invalidations == n);  // identical prices
  m->on_event({ld::EventKind::Tap});
  ctx.respond(2, {0, "", "timeout"});
  CHECK(ctx.invalidations == n + 1);  // went stale
  m->on_event({ld::EventKind::Tap});
  ctx.respond(3, {200, kBody, ""});
  CHECK(ctx.invalidations == n + 2);  // recovered colour
}

TEST_CASE("crypto_gold: two lines, stale together; no settings") {
  FakeLoop loop;
  FakeContext ctx(loop);
  auto m = start(ctx);
  REQUIRE(fixtures::element_of(*m, "status"));  // waiting: a dash
  CHECK(fixtures::element_of(*m, "status")->state == ld::ElementState::Idle);
  ctx.respond(0, {200, kBody, ""});
  CHECK_FALSE(fixtures::element_of(*m, "status"));
  CHECK(fixtures::element_of(*m, "symbol1")->text == L"₿");
  CHECK(fixtures::element_of(*m, "line1")->text == L"98 512");  // no "€"
  CHECK(fixtures::element_of(*m, "line2")->text == L"2 432");
  m->on_event({ld::EventKind::Tap});
  ctx.respond(1, {0, "", "timeout"});
  CHECK(fixtures::element_of(*m, "line1")->state == ld::ElementState::Stale);
  CHECK(fixtures::element_of(*m, "line2")->state == ld::ElementState::Stale);
  nlohmann::json s;
  REQUIRE_FALSE(ld::apply_schema(ld::crypto_gold_module_info().settings_schema, nlohmann::json::object(), s));
  CHECK(s.empty());
}

TEST_CASE("crypto_gold: both lines share the size that fits the wider one") {
  ld::Renderer r;
  auto rendered = [&](const char* body) {
    FakeLoop loop;
    FakeContext ctx(loop);
    auto m = start(ctx);
    ctx.respond(0, {200, body, ""});
    return draw(*m, r);
  };
  auto normal = rendered(kBody);
  // A bitcoin price too long for 26 px: its number shrinks, and the gold number with it (the
  // engine shares the size of the two lines). The gold number only: right of the "Au" symbol.
  auto shrunk = rendered(R"({"bitcoin":{"eur":123456789012},"tether-gold":{"eur":2431.7}})");
  const Ink before = ink(*normal, 36, 58, -1, 104);
  const Ink after = ink(*shrunk, 36, 58, -1, 104);
  REQUIRE(before.count > 0);
  REQUIRE(after.count > 0);
  CHECK(after.height() < before.height());
  CHECK(after.right - after.left < before.right - before.left);
}
