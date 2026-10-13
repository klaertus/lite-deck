#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include "core/config_service.h"
#include "core/config_watcher.h"
#include "core/event_loop.h"
#include "core/strings.h"
#include "layout/deck.h"
#include "device/registry.h"
#include "web/httplib_wrap.h"
#include "web/web_ui.h"
#include "web_test_util.h"

using namespace std::chrono_literals;
using nlohmann::json;
using namespace testutil;
namespace fs = std::filesystem;

namespace {

struct NullSink : ld::DeckSink {
  void key_image(int, const ld::Image&) override {}
  void strip_image(const ld::Image&) override {}
  void rings(std::span<const ld::Rgb>, std::uint8_t) override {}
};

// What main.cpp wires together, on a temp folder, with the test "city" module.
struct Daemon {
  fs::path dir;
  ld::EventLoop loop;
  ld::Renderer renderer;
  NullSink sink;
  ld::ConfigService config;
  ld::Deck deck;
  ld::WebUi web;
  int port = 0;
  std::string token;

  explicit Daemon(const char* name)
      : dir(prepare(name)), config(dir / "config.json", city_lookup), deck(loop, renderer, sink, ld::default_device().layout),
        web(loop, config, deck, [] { return std::string("simulator"); }) {
    deck.apply(config.load(), city_lookup);
    config.on_applied = [this](const ld::Config&, const ld::Config&) {
      loop.post([this] { deck.apply(config.current(), city_lookup); });
    };
    config.on_status = [this] { web.config_changed(); };
    deck.on_frame = [this](int target) { web.frame(target); };
    REQUIRE(web.start(0));
    const std::string url = ld::narrow(web.config_url());
    REQUIRE(url.starts_with("http://127.0.0.1:"));
    port = std::stoi(url.substr(17, url.find('/', 17) - 17));
    token = url.substr(url.find("?t=") + 3);
  }
  ~Daemon() {
    web.stop();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  static fs::path prepare(const char* name) {
    fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    write_text(d / "config.json", doc_with_city("Brussels").dump());
    return d;
  }
  // Logs in like the browser: the token link, then the session cookie. Empty on failure.
  // No doctest assertion here: it also runs on client threads.
  httplib::Headers login() const {
    httplib::Client c("127.0.0.1", port);
    auto r = c.Get("/?t=" + token);
    if (!r || r->status != 303) return {};
    const std::string cookie = r->get_header_value("Set-Cookie");
    return {{"Cookie", cookie.substr(0, cookie.find(';'))}};
  }
};

}  // namespace

TEST_CASE("config_url is the tokenized loopback URL while running, empty after stop") {
  Daemon d("ld_webui_url");
  CHECK(ld::narrow(d.web.config_url()) == "http://127.0.0.1:" + std::to_string(d.port) + "/?t=" + d.token);
  CHECK(d.token.size() == 32);
  d.web.stop();
  CHECK(d.web.config_url().empty());
}

TEST_CASE("a PUT from the page goes through the loop to config.json and the deck") {
  Daemon d("ld_webui_put");
  bool page_ok = false, preview_ok = false;
  int put_status = 0;
  json status;
  std::thread client([&] {
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    if (auto page = c.Get("/", h)) page_ok = page->status == 200 && page->body.find("/app.js") != std::string::npos;
    if (auto cfg = c.Get("/api/config", h); cfg && cfg->status == 200) {
      json doc = json::parse(cfg->body);
      doc["keys"][0]["settings"]["city"] = "Liège";
      httplib::Headers put_headers = h;
      put_headers.emplace("X-Lite-Deck", "1");
      put_headers.emplace("If-Match", cfg->get_header_value("ETag"));
      if (auto put = c.Put("/api/config", put_headers, doc.dump(), "application/json")) put_status = put->status;
    }
    if (auto st = c.Get("/api/status", h); st && st->status == 200) status = json::parse(st->body);
    if (auto pv = c.Get("/api/preview/key/0", h))
      preview_ok = pv->status == 200 && pv->get_header_value("Content-Type") == "image/jpeg";
    d.loop.quit(0);
  });
  d.loop.at(ld::Clock::now() + 10s, [&] { d.loop.quit(1); });
  CHECK(d.loop.run() == 0);
  client.join();
  CHECK(page_ok);
  CHECK(put_status == 200);
  CHECK(status["device"] == "simulator");
  CHECK(status["revision"] == 2);
  CHECK(preview_ok);
  CHECK(d.config.current().keys[0]->settings["city"] == "Liège");
  CHECK(json::parse(read_text(d.dir / "config.json"))["keys"][0]["settings"]["city"] == "Liège");
}

TEST_CASE("an external edit of config.json reaches an open page as a config event") {
  Daemon d("ld_webui_watch");
  ld::ConfigWatcher watcher(d.loop, d.dir / "config.json", [&] { d.config.reload(); });
  std::atomic<bool> attached{false}, got{false};
  std::string seen;
  std::thread reader([&] {
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    c.set_read_timeout(20);
    c.Get("/api/events", h, [&](const char* data, size_t n) {
      attached = true;
      seen.append(data, n);
      if (seen.find("event: config") == std::string::npos) return true;
      got = true;
      return false;  // enough: close the stream
    });
  });
  bool edited = false;
  std::function<void()> tick = [&] {
    if (got) {
      d.loop.quit(0);
      return;
    }
    if (attached && !edited) {
      edited = true;
      write_text(d.dir / "config.json", doc_with_city("Namur").dump());
    }
    d.loop.at(d.loop.now() + 20ms, tick);
  };
  d.loop.at(d.loop.now() + 20ms, tick);
  d.loop.at(d.loop.now() + 10s, [&] { d.loop.quit(1); });
  CHECK(d.loop.run() == 0);
  reader.join();
  CHECK(d.config.current().keys[0]->settings["city"] == "Namur");
  CHECK(seen.find("\"revision\":2") != std::string::npos);
}

TEST_CASE("stop() returns quickly with a page subscribed and a request waiting on the loop") {
  Daemon d("ld_webui_stop");  // the loop never runs: every API call waits on the bridge
  const httplib::Headers h = d.login();  // the token link needs no loop
  REQUIRE_FALSE(h.empty());
  std::atomic<bool> attached{false};
  std::thread sse([&] {
    httplib::Client c("127.0.0.1", d.port);
    c.set_read_timeout(30);
    c.Get("/api/events", h, [&](const char*, size_t) {
      attached = true;
      return true;
    });
  });
  std::thread waiting([&] {
    httplib::Client c("127.0.0.1", d.port);
    c.set_read_timeout(30);
    c.Get("/api/config", h);
  });
  for (int i = 0; i < 200 && !attached; ++i) std::this_thread::sleep_for(10ms);
  REQUIRE(attached);
  std::this_thread::sleep_for(100ms);  // the config request is now waiting on the bridge
  const auto t0 = ld::Clock::now();
  d.web.stop();
  sse.join();
  waiting.join();
  CHECK(ld::Clock::now() - t0 < 1s);  // without LoopBridge::close it would take the 2 s timeout
  CHECK(d.web.config_url().empty());
}

TEST_CASE("GET /api/fonts answers from the cached font list, without the loop") {
  Daemon d("ld_webui_fonts");  // the loop never runs in this test
  const httplib::Headers h = d.login();
  REQUIRE_FALSE(h.empty());
  httplib::Client c("127.0.0.1", d.port);
  auto r = c.Get("/api/fonts", h);
  REQUIRE(r);
  CHECK(r->status == 200);
  json fonts = json::parse(r->body);
  REQUIRE(fonts.is_array());
  CHECK(std::find(fonts.begin(), fonts.end(), "Segoe UI") != fonts.end());
}

TEST_CASE("previews: encoded once, then 304 for the same frame; the encoder is only held while pages are open") {
  Daemon d("ld_webui_previews");
  CHECK_FALSE(d.web.preview_stats().encoder);  // no page has asked yet: no encoder at all
  int first = 0, again = 0;
  std::size_t again_bytes = 1;
  std::string tag;
  std::thread client([&] {
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    if (auto r = c.Get("/api/preview/key/0", h)) {
      first = r->status;
      tag = r->get_header_value("ETag");
    }
    httplib::Headers cond = h;
    cond.emplace("If-None-Match", tag);
    if (auto r = c.Get("/api/preview/key/0", cond)) {
      again = r->status;
      again_bytes = r->body.size();
    }
    d.loop.quit(0);
  });
  d.loop.at(ld::Clock::now() + 10s, [&] { d.loop.quit(1); });
  CHECK(d.loop.run() == 0);
  client.join();
  CHECK(first == 200);
  CHECK_FALSE(tag.empty());
  CHECK(again == 304);
  CHECK(again_bytes == 0);
  const auto s = d.web.preview_stats();
  CHECK(s.encodes == 1);
  CHECK(s.bytes > 0);
  CHECK(s.bytes <= ld::LoopBackend::kMaxPreviewCache);
}

TEST_CASE("previews: the last page closing frees the cache and the encoder, and its old ETag no longer matches") {
  Daemon d("ld_webui_idle");
  std::atomic<bool> attached{false}, leave{false}, checked{false};
  std::string tag;
  int after = 0;
  std::thread page([&] {
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    c.set_read_timeout(30);
    c.Get("/api/events", h, [&](const char*, size_t) {
      attached = true;
      return !leave.load();
    });
  });
  std::thread client([&] {
    while (!attached) std::this_thread::sleep_for(10ms);
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    if (auto r = c.Get("/api/preview/key/0", h)) tag = r->get_header_value("ETag");
    leave = true;  // the page goes: its stream ends at the next ping, 5 s at most
  });
  std::thread late;
  std::function<void()> tick = [&] {
    const auto s = d.web.preview_stats();
    if (leave && s.encodes == 1 && !s.encoder && s.bytes == 0 && !late.joinable()) {
      late = std::thread([&] {  // a page opened later, with the old tag
        httplib::Headers h = d.login();
        h.emplace("If-None-Match", tag);
        httplib::Client c("127.0.0.1", d.port);
        if (auto r = c.Get("/api/preview/key/0", h)) after = r->status;
        checked = true;
      });
    }
    if (checked) {
      d.loop.quit(0);
      return;
    }
    d.loop.at(d.loop.now() + 20ms, tick);
  };
  d.loop.at(d.loop.now() + 20ms, tick);
  d.loop.at(d.loop.now() + 20s, [&] { d.loop.quit(1); });
  CHECK(d.loop.run() == 0);
  page.join();
  client.join();
  if (late.joinable()) late.join();
  CHECK(after == 200);  // a new epoch: the old tag is stale
}

TEST_CASE("previews: with no page open, a preview handed out still follows the frames (a script never gets a stale one)") {
  Daemon d("ld_webui_script");
  std::atomic<bool> got{false}, framed{false};
  std::string tag;
  int again = 0;
  std::thread client([&] {
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    if (auto r = c.Get("/api/preview/key/0", h)) tag = r->get_header_value("ETag");
    got = true;
    while (!framed) std::this_thread::sleep_for(10ms);
    httplib::Headers cond = h;
    cond.emplace("If-None-Match", tag);
    if (auto r = c.Get("/api/preview/key/0", cond)) again = r->status;
    d.loop.quit(0);
  });
  std::function<void()> tick = [&] {
    if (got && !framed) {
      d.web.frame(0);  // key 0 draws a new frame; no event stream is open
      framed = true;
    }
    d.loop.at(d.loop.now() + 10ms, tick);
  };
  d.loop.at(d.loop.now() + 10ms, tick);
  d.loop.at(d.loop.now() + 10s, [&] { d.loop.quit(1); });
  CHECK(d.loop.run() == 0);
  client.join();
  CHECK_FALSE(tag.empty());
  CHECK(again == 200);  // the new frame, not a 304 for the old one
}

TEST_CASE("previews: with no page open, the cache and the encoder go a while after the last preview asked for") {
  Daemon d("ld_webui_script_idle");
  d.web.set_preview_idle(1s);
  std::atomic<bool> done{false};
  std::string tag;
  int after = 0;
  bool held = false, kept_while_asked = true;
  std::thread client([&] {
    const httplib::Headers h = d.login();
    httplib::Client c("127.0.0.1", d.port);
    for (int i = 0; i < 6; ++i) {  // asked every 100 ms, well under the 1 s delay: nothing goes
      if (auto r = c.Get("/api/preview/key/0", h)) tag = r->get_header_value("ETag");
      std::this_thread::sleep_for(100ms);
    }
    done = true;
  });
  std::thread late;
  std::atomic<bool> checked{false};
  std::function<void()> tick = [&] {
    const auto s = d.web.preview_stats();
    if (s.encodes > 0 && !done) {
      held = held || s.encoder;
      kept_while_asked = kept_while_asked && s.encoder && s.bytes > 0;
    }
    if (done && !s.encoder && s.bytes == 0 && !late.joinable()) {
      late = std::thread([&] {  // the script comes back with its old tag: a new epoch
        httplib::Headers h = d.login();
        h.emplace("If-None-Match", tag);
        httplib::Client c("127.0.0.1", d.port);
        if (auto r = c.Get("/api/preview/key/0", h)) after = r->status;
        checked = true;
      });
    }
    if (checked) {
      d.loop.quit(0);
      return;
    }
    d.loop.at(d.loop.now() + 20ms, tick);
  };
  d.loop.at(d.loop.now() + 20ms, tick);
  d.loop.at(d.loop.now() + 10s, [&] { d.loop.quit(1); });
  CHECK(d.loop.run() == 0);
  client.join();
  if (late.joinable()) late.join();
  CHECK(held);
  CHECK(kept_while_asked);
  CHECK(after == 200);
}
