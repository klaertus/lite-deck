#include <doctest/doctest.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "device/registry.h"
#include "fake_web_backend.h"
#include "web/event_hub.h"
#include "web/httplib_wrap.h"
#include "web/server.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
const std::string kToken = "fedcba9876543210fedcba9876543210";

struct Served {
  FakeWebBackend backend;
  std::vector<ld::ModuleInfo> modules;
  ld::WebApi api{kToken, backend, modules, nullptr, ld::default_device().layout};
  ld::EventHub hub;
  ld::WebServer server{api, hub};
  int port = 0;
  Served() {
    REQUIRE(server.start(0));
    port = server.port();
  }
  httplib::Client client() const {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(2);
    c.set_read_timeout(5);
    return c;
  }
  static httplib::Headers auth() { return {{"Cookie", "ld=" + kToken}}; }
};

template <class Pred>
bool wait_for(Pred pred) {
  const auto end = ld::Clock::now() + 2s;
  while (!pred()) {
    if (ld::Clock::now() > end) return false;
    std::this_thread::sleep_for(10ms);
  }
  return true;
}
}  // namespace

TEST_CASE("the server answers on 127.0.0.1 and applies the API's rules") {
  Served s;
  CHECK(s.server.running());
  CHECK(s.api.port() == s.port);
  auto c = s.client();
  auto ok = c.Get("/api/modules", Served::auth());
  REQUIRE(ok);
  CHECK(ok->status == 200);
  auto anon = c.Get("/api/modules");
  REQUIRE(anon);
  CHECK(anon->status == 401);
  auto evil = c.Get("/api/modules", httplib::Headers{{"Cookie", "ld=" + kToken}, {"Host", "evil.example:" + std::to_string(s.port)}});
  REQUIRE(evil);
  CHECK(evil->status == 403);
  auto login = c.Get("/?t=" + kToken);
  REQUIRE(login);
  CHECK(login->status == 303);
  CHECK(login->get_header_value("Set-Cookie").starts_with("ld=" + kToken + ";"));
}

TEST_CASE("a PUT through the socket reaches the API with its UTF-8 intact") {
  Served s;
  httplib::Headers h = Served::auth();
  h.emplace("X-Lite-Deck", "1");
  const std::string city = "Liège - 東京";
  auto r = s.client().Put("/api/config", h, json{{"city", city}}.dump(), "application/json");
  REQUIRE(r);
  CHECK(r->status == 200);
  std::lock_guard lock(s.backend.mu);
  CHECK(s.backend.last_doc["city"].get<std::string>() == city);
}

TEST_CASE("an oversized body is refused with 413 before any handler") {
  Served s;
  httplib::Headers h = Served::auth();
  h.emplace("X-Lite-Deck", "1");
  auto r = s.client().Put("/api/config", h, std::string(ld::WebApi::kMaxBody + 1024, ' '), "application/json");
  CHECK((!r || r->status == 413));  // 413, or the connection is cut while the body is still on its way
  std::lock_guard lock(s.backend.mu);
  CHECK(s.backend.update_calls == 0);
}

TEST_CASE("a second server on the same port fails cleanly") {
  Served s;
  FakeWebBackend backend2;
  std::vector<ld::ModuleInfo> none;
  ld::WebApi api2{kToken, backend2, none, nullptr, ld::default_device().layout};
  ld::EventHub hub2;
  ld::WebServer other(api2, hub2);
  CHECK_FALSE(other.start(s.port));
  CHECK_FALSE(other.running());
  auto r = s.client().Get("/api/modules", Served::auth());
  REQUIRE(r);
  CHECK(r->status == 200);  // the first one still serves
}

TEST_CASE("a third event stream is refused with 503") {
  Served s;
  auto a = s.hub.subscribe();
  auto b = s.hub.subscribe();
  auto r = s.client().Get("/api/events", Served::auth());
  REQUIRE(r);
  CHECK(r->status == 503);
  CHECK(r->get_header_value("X-Content-Type-Options") == "nosniff");
  CHECK(r->get_header_value("X-Frame-Options") == "DENY");
  CHECK(r->get_header_value("Referrer-Policy") == "no-referrer");
}

TEST_CASE("a trailing-dot Host and a duplicate Host are refused with 403") {
  Served s;
  auto c = s.client();
  auto dot = c.Get("/api/modules", httplib::Headers{{"Cookie", "ld=" + kToken}, {"Host", "localhost.:" + std::to_string(s.port)}});
  REQUIRE(dot);
  CHECK(dot->status == 403);
  const std::string good = "127.0.0.1:" + std::to_string(s.port);
  auto dup = c.Get("/api/modules", httplib::Headers{{"Cookie", "ld=" + kToken}, {"Host", "evil.example"}, {"Host", good}});
  REQUIRE(dup);
  CHECK(dup->status == 403);
  auto dup2 = c.Get("/api/modules", httplib::Headers{{"Cookie", "ld=" + kToken}, {"Host", good}, {"Host", "evil.example"}});
  REQUIRE(dup2);
  CHECK(dup2->status == 403);
}

TEST_CASE("the event stream delivers hub events and ends when the server stops") {
  Served s;
  std::mutex mu;
  std::string got;
  std::thread reader([&] {
    auto c = s.client();
    c.set_read_timeout(30);
    c.Get("/api/events", Served::auth(), [&](const char* data, size_t n) {
      std::lock_guard lock(mu);
      got.append(data, n);
      return true;
    });
  });
  REQUIRE(wait_for([&] { return s.hub.streams() == 1; }));
  s.hub.publish("device", {{"state", "disconnected"}});
  CHECK(wait_for([&] {
    std::lock_guard lock(mu);
    return got.find("event: device") != std::string::npos;
  }));
  const auto t0 = ld::Clock::now();
  s.server.stop();
  reader.join();
  CHECK(ld::Clock::now() - t0 < 2s);
  CHECK_FALSE(s.server.running());
  std::lock_guard lock(mu);
  CHECK(got.starts_with("retry: 5000\n\n"));
  CHECK(got.find("data: {\"state\":\"disconnected\"}") != std::string::npos);
}

namespace {
// A connection that sends nothing, like a browser's speculative pre-connection.
SOCKET idle_connection(int port) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  REQUIRE(connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
  return s;
}
}  // namespace

TEST_CASE("the server has one thread per event stream plus two for the API") {
  CHECK(ld::WebServer::kThreads == ld::EventHub::kMaxStreams + 2);
}

TEST_CASE("idle connections give their thread back within a second: two pages open and two pre-connections do not stall the API") {
  Served s;
  std::atomic<bool> leave{false};
  std::vector<std::thread> pages;
  for (int i = 0; i < ld::EventHub::kMaxStreams; ++i) {
    pages.emplace_back([&] {
      auto c = s.client();
      c.set_read_timeout(30);
      c.Get("/api/events", Served::auth(), [&](const char*, size_t) { return !leave.load(); });
    });
  }
  REQUIRE(wait_for([&] { return s.hub.streams() == ld::EventHub::kMaxStreams; }));
  const SOCKET a = idle_connection(s.port);
  const SOCKET b = idle_connection(s.port);
  std::this_thread::sleep_for(100ms);  // both now hold the two API threads
  const auto t0 = ld::Clock::now();
  auto r = s.client().Get("/api/modules", Served::auth());
  const auto took = ld::Clock::now() - t0;
  const bool answered = r && r->status == 200;
  closesocket(a);
  closesocket(b);
  leave = true;
  s.server.stop();
  for (auto& t : pages) t.join();
  CHECK(answered);
  CHECK(took < 1500ms);  // httplib's default keep-alive wait (5 s) would hold the request that long
}
