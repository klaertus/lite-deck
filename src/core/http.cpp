#include "core/http.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <format>
#include "core/log.h"
#include "core/strings.h"

#pragma comment(lib, "winhttp.lib")

namespace ld {

std::string url_encode(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~')
      out.push_back(static_cast<char>(c));
    else
      out += std::format("%{:02X}", c);
  }
  return out;
}

bool valid_http_header(std::string_view name, std::string_view value) {
  if (name.empty()) return false;
  for (unsigned char c : name) {
    const bool token = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    if (!token) return false;
  }
  for (char c : value)
    if (c == '\r' || c == '\n' || c == '\0') return false;
  return true;
}

std::string url_for_log(std::string_view url) {
  url = url.substr(0, url.find_first_of("?#"));
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos) return std::string(url);
  const std::size_t host_at = scheme + 3;
  const std::size_t path_at = std::min(url.find('/', host_at), url.size());
  std::string_view authority = url.substr(host_at, path_at - host_at);
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority = authority.substr(at + 1);
  return std::string(url.substr(0, host_at)) + std::string(authority) + std::string(url.substr(path_at));
}

namespace {
constexpr int kTimeoutMs = 10000;
constexpr std::size_t kMaxBody = 1 << 20;

std::string last_error(const char* what) { return std::format("{} failed ({})", what, GetLastError()); }

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() { if (h) WinHttpCloseHandle(h); }
};
}  // namespace

HttpResponse http_get_blocking(const HttpRequest& request, std::atomic<void*>* cancel_slot) {
  HttpResponse res;
  if (request.header && !valid_http_header(request.header->first, request.header->second))
    return {0, "", "invalid header"};
  std::wstring wurl = widen(request.url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {0, "", last_error("WinHttpCrackUrl")};
  const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && !(request.allow_plain_http && uc.nScheme == INTERNET_SCHEME_HTTP)) return {0, "", "https required"};
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

  Handle session{WinHttpOpen(L"lite-deck/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h) return {0, "", last_error("WinHttpOpen")};
  WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  Handle conn{WinHttpConnect(session.h, host.c_str(), uc.nPort, 0)};
  if (!conn.h) return {0, "", last_error("WinHttpConnect")};
  Handle req{WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
  if (!req.h) return {0, "", last_error("WinHttpOpenRequest")};
  if (request.header) {  // the header goes to this host only, never along a redirect
    DWORD off = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
  }
  // Publish the request handle: closing it from another thread makes the blocking calls below fail
  // at once. On exit, take it back; if someone else already took (and closed) it, do not close twice.
  struct Unpublish {
    std::atomic<void*>* slot;
    Handle& req;
    ~Unpublish() {
      if (slot && slot->exchange(nullptr) != req.h) req.h = nullptr;
    }
  };
  if (cancel_slot) cancel_slot->store(req.h);
  Unpublish unpublish{cancel_slot, req};
  const std::wstring header =
      request.header ? widen(request.header->first + ": " + request.header->second) : std::wstring();
  if (!WinHttpSendRequest(req.h, header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(),
                          header.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr))
    return {0, "", last_error("WinHttpSendRequest/ReceiveResponse")};
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  res.status = static_cast<int>(status);
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail)) return {res.status, "", last_error("WinHttpQueryDataAvailable")};
    if (avail == 0) break;
    if (res.body.size() + avail > kMaxBody) return {res.status, "", kResponseTooLarge};
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(req.h, chunk.data(), avail, &read)) return {res.status, "", last_error("WinHttpReadData")};
    res.body.append(chunk.data(), read);
  }
  return res;
}

struct WinHttpClient::Core {
  struct Job {
    HttpRequest request;
    std::function<void(HttpResponse)> done;
    std::function<bool()> wanted;
  };
  struct Worker {
    std::thread thread;
    std::atomic<void*> current{nullptr};  // request handle in flight; the destructor closes it
    bool exited = false;                  // under mu: the thread is about to return
  };
  struct Lane {
    int max_workers = 1;
    std::deque<Job> queue;
    std::vector<std::unique_ptr<Worker>> workers;
  };

  Core(LoopApi& l, HttpTransport t) : loop(l), transport(std::move(t)) {}
  void run(Lane& lane, Worker& self);
  // Closes the handles in flight: their blocking calls fail at once. Under mu.
  void abort_requests() {
    for (Lane* lane : {&shared, &isolated})
      for (auto& w : lane->workers)
        if (void* h = w->current.exchange(nullptr)) WinHttpCloseHandle(static_cast<HINTERNET>(h));
  }
  bool all_exited() const {
    for (const Lane* lane : {&shared, &isolated})
      for (const auto& w : lane->workers)
        if (!w->exited) return false;
    return true;
  }

  LoopApi& loop;  // only used under mu while !stop: the client is destroyed before the loop
  HttpTransport transport;
  std::mutex mu;
  std::condition_variable cv;       // a job or stop
  std::condition_variable exit_cv;  // a worker exited
  Lane shared, isolated;
  bool stop = false;
};

WinHttpClient::WinHttpClient(LoopApi& loop, HttpTransport transport) {
  if (!transport)
    transport = [](const HttpRequest& r, std::atomic<void*>* slot) { return http_get_blocking(r, slot); };
  core_ = std::make_shared<Core>(loop, std::move(transport));
  core_->isolated.max_workers = kIsolatedWorkers;
}

WinHttpClient::~WinHttpClient() {
  Core& c = *core_;
  std::unique_lock lock(c.mu);
  c.stop = true;
  c.abort_requests();
  c.cv.notify_all();
  // A request published just after the first pass (its worker was between two WinHTTP calls) is
  // closed by a later one: the wait wakes every 50 ms until the workers are out or kStopWait is over.
  const auto deadline = std::chrono::steady_clock::now() + kStopWait;
  while (!c.all_exited() && std::chrono::steady_clock::now() < deadline) {
    c.exit_cv.wait_for(lock, std::chrono::milliseconds(50));
    c.abort_requests();
  }
  const bool all_out = c.all_exited();
  if (!all_out) log_warn("http: a request did not stop within {} ms; leaving it to end on its own", kStopWait.count());
  for (Core::Lane* lane : {&c.shared, &c.isolated})
    for (auto& w : lane->workers) {
      if (!w->thread.joinable()) continue;
      if (w->exited) {
        lock.unlock();
        w->thread.join();  // only returning: it no longer takes the lock
        lock.lock();
      } else {
        w->thread.detach();  // holds `core_` until it ends; it sees `stop` and posts nothing
      }
    }
}

void WinHttpClient::get(const HttpRequest& request, std::function<void(HttpResponse)> done,
                        std::function<bool()> wanted) {
  Core& c = *core_;
  std::lock_guard lock(c.mu);
  Core::Lane& lane = request.lane == HttpLane::Isolated ? c.isolated : c.shared;
  if (lane.queue.size() >= kMaxQueued) {
    log_warn("http: too many requests waiting; {} not sent", url_for_log(request.url));
    c.loop.post([done = std::move(done)]() mutable {
      if (done) done(HttpResponse{0, "", "busy"});
    });
    return;
  }
  lane.queue.push_back(Core::Job{request, std::move(done), std::move(wanted)});
  if (lane.workers.size() < static_cast<std::size_t>(lane.max_workers)) {
    // Workers start with the first requests of their lane, up to its count, and live until shutdown.
    auto& w = lane.workers.emplace_back(std::make_unique<Core::Worker>());
    w->thread = std::thread([core = core_, &lane, &self = *w] {
      core->run(lane, self);
      std::lock_guard exit_lock(core->mu);
      self.exited = true;
      core->exit_cv.notify_all();
    });
  }
  c.cv.notify_all();
}

void WinHttpClient::Core::run(Lane& lane, Worker& self) {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mu);
      cv.wait(lock, [&] { return stop || !lane.queue.empty(); });
      if (stop) return;
      job = std::move(lane.queue.front());
      lane.queue.pop_front();
    }
    try {
      if (job.wanted && !job.wanted()) continue;  // its module is gone: never sent
      HttpResponse res = transport(job.request, &self.current);
      std::lock_guard lock(mu);  // posted under the lock: once `stop` is set, nothing reaches the loop
      if (stop) return;
      loop.post([done = std::move(job.done), res = std::move(res)]() mutable {
        if (done) done(std::move(res));
      });
    } catch (...) {
      log_error("http worker: exception while fetching {}", url_for_log(job.request.url));
      if (!job.done) continue;
      try {
        std::lock_guard lock(mu);
        if (stop) return;
        loop.post([done = std::move(job.done)]() mutable { done(HttpResponse{0, "", "internal error"}); });
      } catch (...) {
        log_error("http worker: cannot post error for {}", url_for_log(job.request.url));
      }
    }
  }
}

}  // namespace ld
