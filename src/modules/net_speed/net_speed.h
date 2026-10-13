// src/modules/net_speed/net_speed.h: download and upload rates of the network interfaces.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "modules/module.h"

namespace ld {

// The byte counters of one interface (64-bit, from GetIfTable2); `id` is its LUID.
struct NetCounter {
  std::uint64_t id = 0;
  std::uint64_t rx = 0, tx = 0;
};
// The interfaces selected by the setting, or nullopt when the table cannot be read.
using NetReader = std::function<std::optional<std::vector<NetCounter>>()>;

struct NetDelta {
  std::uint64_t rx = 0, tx = 0;
};
// The bytes moved between two readings, summed over the interfaces present in both; nullopt when a
// counter went backwards (wrapped, or the adapter was reset): that second is skipped.
std::optional<NetDelta> net_delta(const std::vector<NetCounter>& before, const std::vector<NetCounter>& after);

// "2.1" + "MB/s": B/KB/MB/GB per second (1000 steps), one decimal below 100 (none for B/s); `bits`
// gives b/Kb/Mb/Gb per second.
struct RateText {
  std::wstring number, unit;
};
RateText format_rate(double bytes_per_second, bool bits);

// What decides whether an interface is counted.
struct NetInterfaceInfo {
  std::wstring alias;        // "Ethernet", "Wi-Fi"
  std::wstring description;  // "Intel(R) Ethernet Controller I225-V"
  bool hardware = false;     // InterfaceAndOperStatusFlags.HardwareInterface
  bool up = false;           // OperStatus == IfOperStatusUp
  bool loopback = false;
  bool tunnel = false;
  bool filter = false;       // a filter row repeating an adapter (WFP, QoS)
};
// `which` = "auto": the physical interfaces that are up, without loopback, tunnels and the usual
// virtual adapters (Hyper-V, VirtualBox, VMware, TAP, ...). Otherwise the interface whose alias or
// description is `which`, ignoring case.
bool net_interface_selected(const NetInterfaceInfo& itf, const std::string& which);
// The real reader: GetIfTable2, filtered by net_interface_selected.
NetReader if_table_reader(std::string which);

// An empty reader: if_table_reader(settings "interface").
std::unique_ptr<Module> make_net_speed_module(NetReader reader);
ModuleInfo net_speed_module_info();

}  // namespace ld
