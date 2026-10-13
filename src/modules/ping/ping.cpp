// src/modules/ping/ping.cpp
#include "modules/common/tap_action.h"
#include "modules/ping/ping.h"
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <icmpapi.h>
// clang-format on
#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <vector>
#include "core/log.h"
#include "core/strings.h"
#include "modules/common/format.h"
#include "modules/common/key_look.h"
#include "modules/common/state_settings.h"
#include "modules/key_module.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace ld {

std::wstring format_latency(int ms) { return ms < 1 ? std::wstring(L"<1 ms") : std::format(L"{} ms", ms); }

namespace {

void ensure_winsock() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);  // never cleaned up: the process keeps it
  });
}

std::optional<std::string> address_text(const SOCKADDR_INET& a) {
  wchar_t buf[INET6_ADDRSTRLEN] = {};
  const void* raw = a.si_family == AF_INET6 ? static_cast<const void*>(&a.Ipv6.sin6_addr)
                                            : static_cast<const void*>(&a.Ipv4.sin_addr);
  if (!InetNtopW(a.si_family, raw, buf, INET6_ADDRSTRLEN)) return std::nullopt;
  return narrow(buf);
}

// The next hop of the best IPv4 route to the internet; nullopt when there is none or it is on-link.
std::optional<std::string> default_gateway() {
  SOCKADDR_INET dest{};
  dest.Ipv4.sin_family = AF_INET;
  InetPtonW(AF_INET, L"1.1.1.1", &dest.Ipv4.sin_addr);
  MIB_IPFORWARD_ROW2 route{};
  SOCKADDR_INET source{};
  if (GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &route, &source) != NO_ERROR) return std::nullopt;
  if (route.NextHop.si_family != AF_INET || route.NextHop.Ipv4.sin_addr.s_addr == 0) return std::nullopt;
  return address_text(route.NextHop);
}

}  // namespace

std::optional<std::string> resolve_ping_host(const std::string& host) {
  if (host == "gateway") return default_gateway();
  ensure_winsock();
  ADDRINFOW hints{};
  hints.ai_family = AF_UNSPEC;
  ADDRINFOW* res = nullptr;
  if (GetAddrInfoW(widen(host).c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
  std::optional<std::string> v4, v6;
  for (const ADDRINFOW* p = res; p; p = p->ai_next) {
    SOCKADDR_INET a{};
    if (p->ai_family == AF_INET && !v4) {
      a.Ipv4 = *reinterpret_cast<const sockaddr_in*>(p->ai_addr);
      v4 = address_text(a);
    } else if (p->ai_family == AF_INET6 && !v6) {
      a.Ipv6 = *reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
      v6 = address_text(a);
    }
  }
  FreeAddrInfoW(res);
  return v4 ? v4 : v6;  // IPv4 first: the usual path, and the gateway is IPv4
}

std::optional<int> icmp_echo(const std::string& address, int timeout_ms) {
  static constexpr char kData[32] = "lite-deck ping";
  const std::wstring w = widen(address);
  IN_ADDR v4{};
  IN6_ADDR v6{};
  if (InetPtonW(AF_INET, w.c_str(), &v4) == 1) {
    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(kData) + 8 + 16);  // + room for an IO_STATUS_BLOCK
    const DWORD n = IcmpSendEcho2(h, nullptr, nullptr, nullptr, v4.S_un.S_addr, const_cast<char*>(kData),
                                  sizeof(kData), nullptr, reply.data(), static_cast<DWORD>(reply.size()),
                                  static_cast<DWORD>(timeout_ms));
    IcmpCloseHandle(h);
    const auto* r = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
    if (n == 0 || r->Status != IP_SUCCESS) return std::nullopt;
    return static_cast<int>(r->RoundTripTime);
  }
  if (InetPtonW(AF_INET6, w.c_str(), &v6) == 1) {
    HANDLE h = Icmp6CreateFile();
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    sockaddr_in6 src{};
    src.sin6_family = AF_INET6;
    sockaddr_in6 dst{};
    dst.sin6_family = AF_INET6;
    dst.sin6_addr = v6;
    std::vector<unsigned char> reply(sizeof(ICMPV6_ECHO_REPLY) + sizeof(kData) + 8 + 16);  // + room for an IO_STATUS_BLOCK
    const DWORD n = Icmp6SendEcho2(h, nullptr, nullptr, nullptr, &src, &dst, const_cast<char*>(kData), sizeof(kData),
                                   nullptr, reply.data(), static_cast<DWORD>(reply.size()),
                                   static_cast<DWORD>(timeout_ms));
    IcmpCloseHandle(h);
    const auto* r = reinterpret_cast<const ICMPV6_ECHO_REPLY*>(reply.data());
    if (n == 0 || r->Status != IP_SUCCESS) return std::nullopt;
    return static_cast<int>(r->RoundTripTime);
  }
  return std::nullopt;
}

namespace {

constexpr std::size_t kLossWindow = 20;
constexpr Duration kNameTtl{5 * 60 * 1000};
constexpr Duration kGatewayTtl{60 * 1000};
constexpr Rgb kLime{0xa6, 0xe2, 0x2e};
constexpr ElementSpec kElements[] = {
    {.key = "label", .label = "Label", .size = 17, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Top,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Label},
    {.key = "target", .label = "Target", .size = 15, .weight = 600, .min_size = 8, .max_size = 30, .row = Row::Top,
     .align = HAlign::Right, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
    {.key = "value", .label = "Latency", .size = 36, .weight = 700, .min_size = 10, .max_size = 80, .row = Row::Middle,
     .color_mode = ColorMode::State, .color = kLime, .role = TextRole::Value},
    {.key = "graph", .label = "Graph", .size = 8, .weight = 400, .min_size = 8, .max_size = 8,
     .kind = ElementKind::Graph, .row = Row::Bottom, .color = kLime},
    // Over the graph, at its oldest end (drawn after it).
    {.key = "loss", .label = "Loss", .size = 15, .weight = 600, .min_size = 8, .max_size = 40, .row = Row::Bottom,
     .align = HAlign::Left, .color = kSecondaryGrey, .keeps_color = true, .role = TextRole::Secondary},
};

std::string trimmed(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

class PingModule final : public TapKey {
public:
  PingModule(PingResolver resolver, PingEcho echo, Runner runner)
      : TapKey(kElements), resolver_(std::move(resolver)), echo_(std::move(echo)), runner_(std::move(runner)) {}
  void start(ModuleContext& ctx, const nlohmann::json& s) override {
    start_tap(ctx, s);
    ctx_ = &ctx;
    host_ = trimmed(s.at("host").get<std::string>());
    const int interval = s.at("interval_s").get<int>();
    timeout_ms_ = s.at("timeout_ms").get<int>();
    warn_ = s.at("warn_ms").get<double>();
    hysteresis_ = s.at("hysteresis").get<double>();
    capacity_ = std::max<std::size_t>(2, static_cast<std::size_t>(60 / interval));
    if (!resolver_) resolver_ = resolve_ping_host;
    if (!echo_) echo_ = icmp_echo;
    if (!runner_) runner_ = worker_runner();
    ctx.invalidate();
    if (host_.empty()) return;
    probe();
    ctx.every(Duration(interval * 1000), [this] { probe(); });
  }
  std::vector<KeyElement> content() override {
    std::vector<KeyElement> out;
    out.push_back(KeyElement{.key = "label", .text = L"PING"});
    if (host_.empty()) {
      out.push_back(KeyElement{.key = "value", .text = L"Host?", .state = ElementState::Idle});
      return out;
    }
    out.push_back(KeyElement{.key = "target", .text = widen(host_)});
    KeyElement value{.key = "value", .text = kDash, .state = ElementState::Stale};
    if (!unresolved_ && !loss_.empty()) {
      value.state = state_;
      if (rtt_) value.text = format_latency(*rtt_);
    }
    out.push_back(std::move(value));
    if (loss_.empty()) return out;
    double hi = warn_;
    for (float v : history_) hi = std::max(hi, static_cast<double>(v));
    out.push_back(KeyElement{.key = "graph", .points = graph_points(history_, 0.f, static_cast<float>(hi)),
                             .capacity = capacity_});
    const auto lost = std::count(loss_.begin(), loss_.end(), true);
    const long long percent = std::llround(100.0 * static_cast<double>(lost) / static_cast<double>(loss_.size()));
    out.push_back(KeyElement{.key = "loss", .text = std::format(L"loss {} %", percent)});
    return out;
  }

private:
  void probe() {
    if (busy_) return;  // one probe in flight at a time
    busy_ = true;
    const TimePoint now = ctx_->now();
    std::optional<std::string> address = address_;
    if (address && now >= address_until_) address.reset();
    runner_([resolver = resolver_, echo = echo_, host = host_, address, timeout = timeout_ms_, p = ctx_->poster(),
             this]() mutable {
      const bool resolved = !address;
      if (!address) address = resolver(host);
      std::optional<int> rtt;
      if (address) rtt = echo(*address, timeout);
      p.post([this, address, resolved, rtt] { on_probe(address, resolved, rtt); });
    });
  }
  void on_probe(const std::optional<std::string>& address, bool resolved, std::optional<int> rtt) {
    busy_ = false;
    if (!address) {
      if (!unresolved_) log_info("ping: cannot resolve {}; retrying at each probe", host_);
      unresolved_ = true;
      address_.reset();
      ctx_->invalidate();
      return;
    }
    unresolved_ = false;
    if (resolved) {
      address_ = address;
      address_until_ = ctx_->now() + (host_ == "gateway" ? kGatewayTtl : kNameTtl);
    }
    loss_.push_back(!rtt);
    if (loss_.size() > kLossWindow) loss_.pop_front();
    history_.push_back(static_cast<float>(rtt ? *rtt : timeout_ms_));
    if (history_.size() > capacity_) history_.pop_front();
    rtt_ = rtt;
    if (rtt) {
      const Thresholds t{warn_, std::numeric_limits<double>::infinity(), hysteresis_};
      // After a loss, the latency alone decides: back to warning or normal at once.
      state_ = threshold_state(state_ == ElementState::Critical ? ElementState::Warning : state_, *rtt, t);
    } else {
      state_ = ElementState::Critical;
    }
    ctx_->invalidate();  // the graph moves
  }
  PingResolver resolver_;
  PingEcho echo_;
  Runner runner_;
  ModuleContext* ctx_ = nullptr;
  std::string host_;
  int timeout_ms_ = 1000;
  double warn_ = 80, hysteresis_ = 10;
  std::size_t capacity_ = 30;
  bool busy_ = false;
  bool unresolved_ = false;
  std::optional<std::string> address_;
  TimePoint address_until_{};
  std::optional<int> rtt_;
  ElementState state_ = ElementState::Normal;
  std::deque<bool> loss_;
  std::deque<float> history_;
};

}  // namespace

std::unique_ptr<Module> make_ping_module(PingResolver resolver, PingEcho echo, Runner runner) {
  return std::make_unique<PingModule>(std::move(resolver), std::move(echo), std::move(runner));
}

ModuleInfo ping_module_info() {
  ModuleInfo m;
  m.id = "ping";
  m.name = "Ping";
  m.description = "Delay to an address, and lost packets.";
  m.category = "System";
  m.hue = "#a6e22e";
  m.slots = {SlotKind::Key};
  m.settings_schema = nlohmann::json::parse(R"J({"fields":[
    {"key":"host","type":"string","default":"1.1.1.1","label":"Address","placeholder":"1.1.1.1",
     "hint":"A name, an IP address, or gateway for your router."},
    {"key":"interval_s","type":"int","default":2,"min":1,"max":60,"label":"Every (s)"},
    {"key":"timeout_ms","type":"int","default":1000,"min":200,"max":5000,"label":"Timeout (ms)"},
    {"key":"warn_ms","type":"number","default":80,"min":1,"max":5000,"label":"Warning at (ms)",
     "group":"thresholds"},
    {"key":"hysteresis","type":"number","default":10,"min":0,"max":100,"label":"Clear margin",
     "hint":"Stays in warning until the value is this far back. Avoids flicker.","group":"thresholds"}]})J");
  m.settings_schema = with_fields(std::move(m.settings_schema), tap_fields("task_manager"));
  m.elements.assign(std::begin(kElements), std::end(kElements));
  m.create = [] { return make_ping_module({}, {}, {}); };
  return m;
}

}  // namespace ld
