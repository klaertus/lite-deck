// src/modules/ping/ping.h: the latency to a host (ICMP echo), its loss and a 60-second graph.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "modules/common/worker.h"
#include "modules/module.h"

namespace ld {

// Both run on a worker thread and may block. The resolver turns a host name, an address or
// "gateway" (the default IPv4 gateway) into a numeric address; nullopt when it cannot.
using PingResolver = std::function<std::optional<std::string>(const std::string& host)>;
// One echo request to a numeric address: the round trip in ms, or nullopt (timeout, unreachable).
using PingEcho = std::function<std::optional<int>(const std::string& address, int timeout_ms)>;

std::optional<std::string> resolve_ping_host(const std::string& host);  // GetAddrInfoW, GetBestRoute2
std::optional<int> icmp_echo(const std::string& address, int timeout_ms);  // IcmpSendEcho2, Icmp6SendEcho2

// "12 ms", "<1 ms".
std::wstring format_latency(int ms);

// Empty arguments: resolve_ping_host, icmp_echo and worker_runner().
std::unique_ptr<Module> make_ping_module(PingResolver resolver, PingEcho echo, Runner runner);
ModuleInfo ping_module_info();

}  // namespace ld
