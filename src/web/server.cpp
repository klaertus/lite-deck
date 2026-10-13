// src/web/server.cpp
#include "web/server.h"
#include "web/httplib_wrap.h"
#include <cctype>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include "core/log.h"

namespace ld {
namespace {

WebRequest to_request(const httplib::Request& r) {
  WebRequest w;
  w.method = r.method;
  w.path = r.path;
  w.body = r.body;
  for (const auto& [name, value] : r.headers) {
    std::string key = name;
    for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto [it, inserted] = w.headers.emplace(key, value);  // the first value wins...
    if (!inserted && key == "host") it->second.clear();   // ...but two Host headers are refused (empty Host = 403)
  }
  for (const auto& [name, value] : r.params) w.query[name] = value;
  return w;
}

// The body is moved, not copied, into httplib's response.
void send(WebResponse w, httplib::Response& r) {
  r.status = w.status;
  for (const auto& [name, value] : w.headers) r.set_header(name, value);
  r.set_content(std::move(w.body), w.content_type);
}

// A response built here (not by WebApi) still carries the hardening headers.
void send_error(int status, const char* message, httplib::Response& r) {
  WebResponse w;
  w.status = status;
  w.content_type = "application/json; charset=utf-8";
  w.body = std::string(R"({"errors":[{"path":"","message":")") + message + R"("}]})";
  send(with_security_headers(std::move(w)), r);
}

}  // namespace

struct WebServer::Impl {
  Impl(WebApi& a, EventHub& h) : api(a), hub(h) {}

  void events(const httplib::Request& req, httplib::Response& res) {
    if (auto deny = api.guard(to_request(req))) {
      send(std::move(*deny), res);
      return;
    }
    std::shared_ptr<EventHub::Stream> stream = hub.subscribe();
    if (!stream) {
      send_error(503, "too many pages open", res);
      return;
    }
    for (const auto& [name, value] : with_security_headers(WebResponse{}).headers) res.set_header(name, value);
    res.set_chunked_content_provider("text/event-stream", [stream, first = true](std::size_t, httplib::DataSink& sink) mutable {
      const std::string chunk = first ? std::string("retry: 5000\n\n") : stream->next();
      first = false;
      if (chunk.empty()) {  // hub closed: end the response cleanly
        sink.done();
        return true;
      }
      return sink.write(chunk.data(), chunk.size());  // false = page gone: the stream is dropped
    });
  }

  WebApi& api;
  EventHub& hub;
  httplib::Server svr;
  std::thread thread;
  int port = 0;
};

WebServer::WebServer(WebApi& api, EventHub& hub) : impl_(std::make_unique<Impl>(api, hub)) {
  httplib::Server& svr = impl_->svr;
  svr.new_task_queue = [] { return new httplib::ThreadPool(kThreads, kThreads, 32); };
  svr.set_address_family(AF_INET);
  svr.set_socket_options([](socket_t sock) {
    BOOL on = TRUE;  // nobody else may bind our port while we listen (httplib's default is SO_REUSEADDR)
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
  });
  svr.set_idle_interval(std::chrono::hours(1));  // the accept thread sleeps in WSAPoll; stop() wakes it
  svr.set_keep_alive_max_count(1);               // one request per connection
  // A connection that sends nothing (a browser's speculative pre-connection) holds a thread until
  // this delay: 1 s, not httplib's 5 s, so that two of them cannot stall the API.
  svr.set_keep_alive_timeout(std::chrono::seconds(1));
  svr.set_read_timeout(std::chrono::seconds(5));
  svr.set_write_timeout(std::chrono::seconds(5));
  svr.set_payload_max_length(WebApi::kMaxBody);  // larger bodies are refused (413) before being buffered
  svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
    std::string what = "unknown";
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
    }
    log_error("web: {} {} failed: {}", req.method, req.path, what);
    send_error(500, "internal error", res);
  });
  Impl* self = impl_.get();
  svr.Get("/api/events", [self](const httplib::Request& req, httplib::Response& res) { self->events(req, res); });
  auto handler = [self](const httplib::Request& req, httplib::Response& res) {
    send(self->api.handle(to_request(req)), res);
  };
  svr.Get(R"(/.*)", handler);
  svr.Put(R"(/.*)", handler);
  svr.Post(R"(/.*)", handler);
  svr.Delete(R"(/.*)", handler);
}

WebServer::~WebServer() { stop(); }

bool WebServer::start(int port) {
  Impl& s = *impl_;
  if (s.thread.joinable()) return s.svr.is_running();
  bool bound = false;
  if (port == 0) {
    const int any = s.svr.bind_to_any_port("127.0.0.1");
    bound = any > 0;
    if (bound) port = any;
  } else {
    bound = s.svr.bind_to_port("127.0.0.1", port);
  }
  if (!bound) {
    log_warn("web: cannot listen on 127.0.0.1:{} (port in use?); the web UI is disabled", port);
    return false;
  }
  s.port = port;
  s.api.set_port(port);
  s.thread = std::thread([&svr = s.svr] { svr.listen_after_bind(); });
  s.svr.wait_until_ready();
  log_info("web: listening on 127.0.0.1:{}", port);
  return s.svr.is_running();
}

void WebServer::stop() {
  Impl& s = *impl_;
  s.hub.close();  // wakes the event streams so their threads can end
  s.svr.stop();   // closes the listening socket: the accept loop ends, the pool is joined
  if (s.thread.joinable()) s.thread.join();
}

bool WebServer::running() const { return impl_->svr.is_running(); }
int WebServer::port() const { return impl_->port; }

}  // namespace ld
