#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "core/loop_api.h"

namespace ld {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string error;  // empty on transport success
  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};
// HttpResponse::error of a body above the 1 MB cap: the caller may ask for less.
inline constexpr const char* kResponseTooLarge = "response too large";

// Which workers carry a request. Requests to the well-known public APIs (weather, sun, prices) share
// one lane; requests to a host the user typed (an http_sensor), which may be down and hold a worker
// for the whole timeout, go to their own lane, so they never delay the others.
enum class HttpLane { Shared, Isolated };

// One GET as a module asks it. A url alone is the common case: HttpRequest{.url = u}.
struct HttpRequest {
  std::string url;
  // One extra request header, such as {"Authorization", "Bearer ..."}. Its value is a secret: it is
  // never logged, and redirects are not followed when it is set (it goes to this host only).
  std::optional<std::pair<std::string, std::string>> header;
  bool allow_plain_http = false;  // false: https only (http:// answers the error "https required")
  HttpLane lane = HttpLane::Shared;
};

class HttpClient {
public:
  virtual ~HttpClient() = default;
  // Asynchronous GET. `done` is called later on the loop thread, never inside get(). `wanted`, when
  // given, is asked (on a worker thread, so it must be thread-safe) just before the request starts:
  // false drops it unsent and `done` is never called (its module is gone).
  virtual void get(const HttpRequest& request, std::function<void(HttpResponse)> done,
                   std::function<bool()> wanted) = 0;
  void get(const HttpRequest& request, std::function<void(HttpResponse)> done) {
    get(request, std::move(done), {});
  }
  void get(const std::string& url, std::function<void(HttpResponse)> done) {
    get(HttpRequest{.url = url}, std::move(done), {});
  }
};

std::string url_encode(std::string_view utf8);  // RFC 3986 unreserved characters kept, the rest %XX

// A header name of RFC 7230 token characters, and a value without CR, LF or NUL.
bool valid_http_header(std::string_view name, std::string_view value);
// "scheme://host[:port]/path" of a url: no credentials, no query, no fragment. The only form of a
// url that goes to the log (a request header never does).
std::string url_for_log(std::string_view url);

// Blocking GET (WinHTTP, 10 s timeouts, 1 MB body cap, User-Agent lite-deck/1.0): https only unless
// the request allows plain http; its header, if any, is sent and redirects are then not followed.
// If `cancel_slot` is given, the live request handle is published there so another thread can abort
// the request by exchanging it out and closing it.
HttpResponse http_get_blocking(const HttpRequest& request, std::atomic<void*>* cancel_slot = nullptr);
inline HttpResponse http_get_blocking(const std::string& url, std::atomic<void*>* cancel_slot = nullptr) {
  return http_get_blocking(HttpRequest{.url = url}, cancel_slot);
}

// What runs one request on a worker thread: http_get_blocking, or a fake in the tests. It publishes
// its live WinHTTP handle in `cancel_slot` (the client closes it to abort at shutdown).
using HttpTransport = std::function<HttpResponse(const HttpRequest&, std::atomic<void*>* cancel_slot)>;

// A few worker threads owned by the core, started on the first request of their lane: one for the
// shared lane (requests are rare, minutes apart), kIsolatedWorkers for the isolated lane. A lane holds
// at most kMaxQueued waiting requests; past that a request is answered at once with the error "busy".
// Results are posted back to the loop. Destroy before the LoopApi it posts to. The destructor aborts
// the requests in flight (it closes their WinHTTP handles, also one published while it waits) and
// waits at most kStopWait for the workers: one still blocked after that (a transport that ignores the
// abort) is left to end on its own, and never posts anything once the client is gone.
class WinHttpClient final : public HttpClient {
public:
  static constexpr int kIsolatedWorkers = 2;
  static constexpr std::size_t kMaxQueued = 32;
  static constexpr Duration kStopWait{1000};
  explicit WinHttpClient(LoopApi& loop, HttpTransport transport = {});
  ~WinHttpClient() override;
  WinHttpClient(const WinHttpClient&) = delete;
  WinHttpClient& operator=(const WinHttpClient&) = delete;
  using HttpClient::get;
  void get(const HttpRequest& request, std::function<void(HttpResponse)> done,
           std::function<bool()> wanted) override;

private:
  // Shared with the worker threads: a worker left running after the destructor keeps it alive.
  struct Core;
  std::shared_ptr<Core> core_;
};

}  // namespace ld
