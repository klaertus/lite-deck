// src/web/server.h
#pragma once
#include <memory>
#include "web/api.h"
#include "web/event_hub.h"

namespace ld {

// cpp-httplib on 127.0.0.1 only: one accept thread asleep in WSAPoll and kThreads pooled threads
// asleep on a condition variable until a request comes. Every request goes through WebApi;
// /api/events streams the EventHub. start/stop from the loop thread.
class WebServer {
public:
  // Each open event stream holds a thread; two more serve the API.
  static constexpr int kThreads = EventHub::kMaxStreams + 2;

  WebServer(WebApi& api, EventHub& hub);
  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  // Binds 127.0.0.1:<port> exclusively (0 = any free port, for tests) and starts serving.
  // false (logged) when the port is taken: the daemon then runs without its web UI.
  bool start(int port);
  // Ends the event streams, stops listening, joins every web thread. Idempotent.
  // Close the LoopBridge first, so no request is left waiting on the loop.
  void stop();
  bool running() const;
  int port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ld
