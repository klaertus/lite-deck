#include <doctest/doctest.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "core/event_loop.h"
#include "core/http.h"
#include "core/log.h"

TEST_CASE("url_encode keeps unreserved characters and percent-encodes the rest") {
  CHECK(ld::url_encode("Brussels") == "Brussels");
  CHECK(ld::url_encode("Aix-en-Provence") == "Aix-en-Provence");
  CHECK(ld::url_encode("New York") == "New%20York");
  CHECK(ld::url_encode("Liège") == "Li%C3%A8ge");
  CHECK(ld::url_encode("a&b=c") == "a%26b%3Dc");
}

TEST_CASE("HttpResponse::ok requires 2xx and no error") {
  CHECK(ld::HttpResponse{200, "x", ""}.ok());
  CHECK_FALSE(ld::HttpResponse{429, "", ""}.ok());
  CHECK_FALSE(ld::HttpResponse{200, "", "timeout"}.ok());
}

TEST_CASE("net: http_get_blocking reaches open-meteo" * doctest::skip()) {
  auto r = ld::http_get_blocking("https://api.open-meteo.com/v1/forecast?latitude=50.85&longitude=4.35&current=temperature_2m");
  CHECK(r.ok());
  CHECK(r.body.find("temperature_2m") != std::string::npos);
}

TEST_CASE("WinHttpClient delivers a fast error on the loop and joins on destruction") {
  ld::EventLoop loop;
  std::string error;
  {
    ld::WinHttpClient client(loop);
    client.get("http://example.com/", [&](ld::HttpResponse r) {
      error = r.error;
      loop.quit(0);
    });
    loop.at(ld::Clock::now() + std::chrono::seconds(5), [&] { loop.quit(1); });
    CHECK(loop.run() == 0);
  }
  CHECK(error == "https required");
}

namespace {
// A transport that never touches the network: a request whose url holds "slow" waits until released.
struct GatedTransport {
  std::mutex mu;
  std::condition_variable cv;
  bool released = false;
  std::atomic<int> calls{0};
  std::vector<ld::HttpRequest> seen;
  ld::HttpTransport fn() {
    return [this](const ld::HttpRequest& r, std::atomic<void*>*) {
      std::unique_lock lock(mu);
      ++calls;
      seen.push_back(r);
      if (r.url.find("slow") != std::string::npos) cv.wait(lock, [&] { return released; });
      return ld::HttpResponse{200, r.url, ""};
    };
  }
  void release() {
    {
      std::lock_guard lock(mu);
      released = true;
    }
    cv.notify_all();
  }
};
}  // namespace

TEST_CASE("WinHttpClient: a blocked isolated request does not delay a shared one") {
  ld::EventLoop loop;
  GatedTransport t;
  std::vector<std::string> order;
  {
    ld::WinHttpClient client(loop, t.fn());
    client.get(ld::HttpRequest{.url = "http://sensor/slow", .lane = ld::HttpLane::Isolated},
               [&](ld::HttpResponse r) {
                 order.push_back(r.body);
                 loop.quit(0);
               });
    client.get("https://weather/forecast", [&](ld::HttpResponse r) {
      order.push_back(r.body);
      t.release();  // only now does the sensor answer
    });
    loop.at(ld::Clock::now() + std::chrono::seconds(5), [&] { loop.quit(1); });
    CHECK(loop.run() == 0);
    t.release();  // in case of failure: let the worker finish before the client joins it
  }
  REQUIRE(order.size() == 2);
  CHECK(order[0] == "https://weather/forecast");
  CHECK(order[1] == "http://sensor/slow");
}

TEST_CASE("WinHttpClient: a second isolated worker serves another sensor while one is blocked") {
  ld::EventLoop loop;
  GatedTransport t;
  std::string got;
  {
    ld::WinHttpClient client(loop, t.fn());
    client.get(ld::HttpRequest{.url = "http://a/slow", .lane = ld::HttpLane::Isolated}, [](ld::HttpResponse) {});
    client.get(ld::HttpRequest{.url = "http://b/fast", .lane = ld::HttpLane::Isolated}, [&](ld::HttpResponse r) {
      got = r.body;
      loop.quit(0);
    });
    loop.at(ld::Clock::now() + std::chrono::seconds(5), [&] { loop.quit(1); });
    CHECK(loop.run() == 0);
    t.release();
  }
  CHECK(got == "http://b/fast");
}

TEST_CASE("WinHttpClient: the transport gets the whole request; an unwanted one is never sent") {
  ld::EventLoop loop;
  GatedTransport t;
  int answered = 0;
  {
    ld::WinHttpClient client(loop, t.fn());
    client.get(ld::HttpRequest{.url = "https://gone/x"}, [&](ld::HttpResponse) { ++answered; },
               [] { return false; });
    client.get(ld::HttpRequest{.url = "https://h/p?k=v",
                               .header = std::pair<std::string, std::string>{"Authorization", "Bearer s3cret"},
                               .allow_plain_http = true},
               [&](ld::HttpResponse) {
                 ++answered;
                 loop.quit(0);
               },
               [] { return true; });
    loop.at(ld::Clock::now() + std::chrono::seconds(5), [&] { loop.quit(1); });
    CHECK(loop.run() == 0);
  }
  CHECK(answered == 1);
  REQUIRE(t.seen.size() == 1);
  CHECK(t.seen[0].url == "https://h/p?k=v");
  REQUIRE(t.seen[0].header);
  CHECK(t.seen[0].header->second == "Bearer s3cret");
  CHECK(t.seen[0].allow_plain_http);
}

TEST_CASE("WinHttpClient: a failing request logs its host and path only, never the query or the header") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ld_http_client_log";
  fs::remove_all(dir);
  ld::log_init(dir / "t.log", ld::LogLevel::Debug);
  ld::EventLoop loop;
  std::string error;
  {
    ld::WinHttpClient client(loop, [](const ld::HttpRequest&, std::atomic<void*>*) -> ld::HttpResponse {
      throw std::runtime_error("transport broke");
    });
    client.get(ld::HttpRequest{.url = "https://ha.local/api/states?token=q5ecret",
                               .header = std::pair<std::string, std::string>{"Authorization", "Bearer s3cret"},
                               .lane = ld::HttpLane::Isolated},
               [&](ld::HttpResponse r) {
                 error = r.error;
                 loop.quit(0);
               });
    loop.at(ld::Clock::now() + std::chrono::seconds(5), [&] { loop.quit(1); });
    CHECK(loop.run() == 0);
  }
  ld::log_init({}, ld::LogLevel::Info);
  CHECK(error == "internal error");
  std::stringstream ss;
  ss << std::ifstream(dir / "t.log").rdbuf();
  const std::string log = ss.str();
  CHECK(log.find("https://ha.local/api/states") != std::string::npos);
  CHECK(log.find("q5ecret") == std::string::npos);
  CHECK(log.find("s3cret") == std::string::npos);
  CHECK(log.find("Authorization") == std::string::npos);
  fs::remove_all(dir);
}
