#include "modules/gpu/gpu_stats.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <vector>
#include "core/log.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dxgi.lib")

namespace ld {
namespace {

struct Adapter {
  LUID luid{};
  SIZE_T dedicated = 0;
  D3DKMT_HANDLE handle = 0;  // opened lazily for the temperature query, closed when the list is refreshed
  bool temperature_worked = false;  // a later failure then means a stale handle, not a driver without it
};

// Hardware adapters sorted by dedicated memory (largest first).
std::vector<Adapter> enumerate_adapters() {
  std::vector<Adapter> out;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      out.push_back(Adapter{d.AdapterLuid, d.DedicatedVideoMemory, 0});
    a.Reset();
  }
  std::stable_sort(out.begin(), out.end(), [](const Adapter& x, const Adapter& y) { return x.dedicated > y.dedicated; });
  if (out.empty()) log_info("gpu: no hardware adapter found");
  return out;
}

void close_handles(std::vector<Adapter>& list) {
  for (Adapter& a : list) {
    if (!a.handle) continue;
    D3DKMT_CLOSEADAPTER c{};
    c.hAdapter = a.handle;
    D3DKMTCloseAdapter(&c);
    a.handle = 0;
  }
}

// The adapter list, enumerated on first use and again after repeated failures (GpuRefreshGate). Only
// the loop thread reads the GPU.
struct Adapters {
  std::vector<Adapter> list = enumerate_adapters();
  GpuRefreshGate gate;
};
Adapters& adapters() {
  static Adapters a;
  return a;
}

// Every query reports here, after its last use of an Adapter reference: a refresh replaces the list.
void report(bool ok) {
  Adapters& a = adapters();
  if (ok) {
    a.gate.succeeded();
    return;
  }
  if (!a.gate.failed(std::chrono::steady_clock::now())) return;
  log_info("gpu: repeated D3DKMT failures, enumerating the adapters again");
  close_handles(a.list);
  a.list = enumerate_adapters();
}

std::optional<double> vram_used_gib(const LUID& luid) {
  D3DKMT_QUERYSTATISTICS q{};
  q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
  q.AdapterLuid = luid;
  if (D3DKMTQueryStatistics(&q) != 0) return std::nullopt;
  ULONG segments = q.QueryResult.AdapterInformation.NbSegments;
  ULONGLONG resident = 0;
  for (ULONG i = 0; i < segments; ++i) {
    D3DKMT_QUERYSTATISTICS s{};
    s.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
    s.AdapterLuid = luid;
    s.QuerySegment.SegmentId = i;
    if (D3DKMTQueryStatistics(&s) != 0) continue;
    if (s.QueryResult.SegmentInformation.Aperture == 0) resident += s.QueryResult.SegmentInformation.BytesResident;
  }
  return static_cast<double>(resident) / static_cast<double>(1ull << 30);
}

struct TempResult {
  std::optional<int> celsius;
  bool adapter_failed = false;  // the adapter itself (open, or a query that used to work), not a missing sensor
};

TempResult temperature_c(Adapter& a) {
  if (!a.handle) {
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = a.luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) return {std::nullopt, true};
    a.handle = open.hAdapter;
  }
  D3DKMT_ADAPTER_PERFDATA perf{};
  perf.PhysicalAdapterIndex = 0;
  D3DKMT_QUERYADAPTERINFO info{};
  info.hAdapter = a.handle;
  info.Type = KMTQAITYPE_ADAPTERPERFDATA;
  info.pPrivateDriverData = &perf;
  info.PrivateDriverDataSize = sizeof(perf);
  // Needs WDDM 2.4 and driver support: many iGPUs, older drivers and VMs never answer.
  if (D3DKMTQueryAdapterInfo(&info) != 0) return {std::nullopt, a.temperature_worked};
  a.temperature_worked = true;
  if (perf.Temperature == 0) return {};  // some drivers answer but leave it at zero: unknown
  return {static_cast<int>((perf.Temperature + 5) / 10), false};  // deci-Celsius -> rounded Celsius
}

// RunningTime unit: 100 ns, as LibreHardwareMonitor and System Informer read it (the SDK comment says
// microseconds; the validation compares the result with Task Manager).
constexpr std::int64_t kRunningTimeTo100ns = 1;
constexpr ULONG kMaxNodes = 64;  // far above any real adapter; bounds the loop

std::int64_t qpc_100ns() {
  LARGE_INTEGER count{}, freq{};
  QueryPerformanceCounter(&count);
  QueryPerformanceFrequency(&freq);
  return count.QuadPart / freq.QuadPart * 10'000'000 + count.QuadPart % freq.QuadPart * 10'000'000 / freq.QuadPart;
}

// An index past the end of an empty list counts as a failure (the only adapter went away); past the
// end of a non-empty list it is a setting for a card that is not there.
bool out_of_range(unsigned index) {
  const auto& list = adapters().list;
  if (index < list.size()) return false;
  if (list.empty()) report(false);
  return true;
}

}  // namespace

std::optional<GpuReading> gpu_read(unsigned index, bool with_temperature) {
  if (out_of_range(index)) return std::nullopt;
  Adapter& a = adapters().list[index];
  const auto vram = vram_used_gib(a.luid);
  std::optional<GpuReading> out;
  bool ok = vram.has_value();
  if (vram) {
    out = GpuReading{*vram, std::nullopt, static_cast<double>(a.dedicated) / static_cast<double>(1ull << 30)};
    if (with_temperature) {
      const TempResult t = temperature_c(a);
      out->temp_c = t.celsius;
      ok = !t.adapter_failed;
    }
  }
  report(ok);  // last: may replace the list `a` belongs to
  return out;
}

std::optional<int> gpu_temperature(unsigned index) {
  if (out_of_range(index)) return std::nullopt;
  const TempResult t = temperature_c(adapters().list[index]);
  report(!t.adapter_failed);
  return t.celsius;
}

std::optional<GpuNodeSample> gpu_node_sample(unsigned index) {
  if (out_of_range(index)) return std::nullopt;
  const LUID luid = adapters().list[index].luid;
  D3DKMT_QUERYSTATISTICS q{};
  q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
  q.AdapterLuid = luid;
  if (D3DKMTQueryStatistics(&q) != 0) {
    report(false);
    return std::nullopt;
  }
  const ULONG nodes = q.QueryResult.AdapterInformation.NodeCount;
  if (nodes == 0 || nodes > kMaxNodes) {  // the adapter answers, it just publishes no engines
    report(true);
    return std::nullopt;
  }
  GpuNodeSample s;
  s.running_100ns.reserve(nodes);
  for (ULONG i = 0; i < nodes; ++i) {
    D3DKMT_QUERYSTATISTICS n{};
    n.Type = D3DKMT_QUERYSTATISTICS_NODE;
    n.AdapterLuid = luid;
    n.QueryNode.NodeId = i;
    if (D3DKMTQueryStatistics(&n) != 0) {
      report(false);
      return std::nullopt;
    }
    s.running_100ns.push_back(n.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart *
                              kRunningTimeTo100ns);
  }
  s.wall_100ns = qpc_100ns();
  report(true);
  return s;
}

}  // namespace ld
