# Adding a temperature source

lite-deck reads the CPU temperature of **AMD processors of family 17h to 1Ah** (Ryzen, Zen 1 to Zen 5),
and that is the only source tested on real hardware. The sensors service is built so that someone with
another CPU family, Intel in the first place, can add it without touching the core: a source class and
one line in the source registry. The daemon, the `cpu_temp` and `cpu_load` modules and the shared
memory they read do not change.

This guide covers what a source is, how the service picks one, how Intel package temperature is
usually read with PawnIO, where the module and its licence go, how to test without the hardware, and
what to check with it.

## How the temperature gets to the deck

| Part | Runs as | Role |
|---|---|---|
| `lite-deck-sensors.exe` (`sensors-service/main.cpp`) | the `LiteDeckSensors` service, LocalSystem | picks a source, reads it once a second, publishes the value |
| a temperature source (`src/sensors/`) | inside the service | talks to [PawnIO](https://pawnio.eu/) and one PawnIO module |
| `Global\LiteDeckSensors` (`src/sensors/sensor_block.h`) | shared memory, read only for users | one value, a timestamp and a status code |
| `lite-deck.exe` | the user | shows the value; knows nothing of the CPU family |

Reading a temperature sensor needs kernel access. lite-deck never ships a driver of its own: it uses
the signed PawnIO driver and one of its signed modules (small programs PawnIO runs in the kernel, each
limited to what it was written for). Each source names the module it needs.

## What a source is

A source implements `TempSource` (`src/sensors/publisher.h`):

```cpp
class TempSource {
public:
  virtual ~TempSource() = default;
  virtual std::optional<double> read_celsius() = 0;  // nullopt = the read failed
};
```

and is opened by a function that returns an `OpenResult`: the source, or a status and a detail for
the log.

```cpp
struct OpenResult {
  std::unique_ptr<TempSource> source;  // null on failure
  Status status = Status::Ok;           // why it failed
  std::string detail;                   // HRESULT, path... for the log
};
```

Use the existing status codes (`src/sensors/sensor_block.h`): `PawnIoMissing` (PawnIOLib.dll or the
driver is not installed), `ModuleMissing` (the module file is missing, empty or over 1 MB),
`ModuleRejected` (`pawnio_load` refused it), `AccessDenied` (`pawnio_open` refused), `UnsupportedCpu`
and `ReadFailed`. Do not add codes: the daemon shows them as they are.

The `Publisher` (`src/sensors/publisher.cpp`) does the rest, whatever the source:

- **Opening.** While no source is open, it calls the open function, and again every 30 s while that
  fails. Each change of status is logged once.
- **Reading.** Once a second, it calls `read_celsius()`. A value outside -20 to 150 °C, a `nullopt` or
  an exception closes the source (`ReadFailed`) and it is opened again 30 s later. So `read_celsius()`
  must not block for long, and returns `nullopt` rather than a guess.

## How the service picks a source

The registry is `src/sensors/temp_sources.h`. Each entry is:

```cpp
struct TempSourceEntry {
  std::string id;      // stable, written in the log: "amd-family17" (a-z, 0-9 and '-')
  std::string name;    // written in the log: "AMD family 17h-1Ah (Tctl/Tdie)"
  std::string module;  // the PawnIO module it loads, a file next to lite-deck-sensors.exe: "AMDFamily17.bin"
  std::function<bool(const CpuIdent&)> matches;
  std::function<OpenResult(const std::filesystem::path& module_file, const CpuIdent& cpu)> open;
};
```

At startup the service:

1. reads the CPU identity once (`read_cpu_ident()`, `src/sensors/cpu_ident.h`: the CPUID vendor
   string, brand string, display family and model);
2. checks the registry (`sound_temp_sources`): an entry with an invalid id, an empty name, a module
   that is not a plain `.bin` file name, a missing function or an id already used is left out, and the
   reason is logged;
3. takes the **first** entry whose `matches(cpu)` is true (`choose_temp_source`) and logs it:
   `sensors: started (source amd-family17 - AMD family 17h-1Ah (Tctl/Tdie), module C:\Program Files\lite-deck\AMDFamily17.bin)`;
4. hands the `Publisher` an opener that calls that entry's `open` with the full path of its module in
   the service's own folder.

When no entry matches, the service publishes `UnsupportedCpu` with the detail
`<vendor> family <family>h` (for example `GenuineIntel family 6h`), and the deck shows that status.

| Step | File |
|---|---|
| Write the source | `src/sensors/<family>_source.h` and `.cpp`, plus pure helpers (register decoding) in their own file. CMake picks up every `src/sensors/*.cpp`, so `CMakeLists.txt` does not change. |
| Register it | Add `#include "sensors/<family>_source.h"` and `<family>_entry(),` to `builtin_temp_sources()` in `src/sensors/temp_sources.cpp`. |
| Ship its module | `third_party/pawnio-modules/<Module>.bin`, its entry in `SOURCE.txt`, and its licence (see [The module and its licence](#the-module-and-its-licence)). |
| Test it | `tests/test_<family>_source.cpp` (see [Testing](#testing-without-the-hardware)). |

The AMD source is the reference: `PawnIoSource` and `amd_family17_entry()` in
`src/sensors/pawnio_source.*`, with the register decoding in `src/sensors/tctl.*`.

### Registering the source

```cpp
// src/sensors/temp_sources.cpp
std::vector<TempSourceEntry> builtin_temp_sources() {
  return {
      amd_family17_entry(),
      intel_package_entry(),  // your source
  };
}
```

```cpp
TempSourceEntry intel_package_entry() {
  return TempSourceEntry{
      .id = "intel-package",
      .name = "Intel package temperature (IA32_PACKAGE_THERM_STATUS)",
      .module = "IntelMSR.bin",
      .matches = [](const CpuIdent& cpu) { return cpu.vendor == "GenuineIntel" && cpu.family == 6; },
      .open = [](const std::filesystem::path& module_file, const CpuIdent& cpu) {
        return IntelPackageSource::open(module_file, cpu);
      },
  };
}
```

Keep `matches` cheap and free of side effects: it only looks at the `CpuIdent`. Anything that needs
more (a CPUID feature bit, a model list), check again in `open` and return `UnsupportedCpu` there.
Order matters only when two entries match the same CPU: put the more specific one first.

## Reading Intel package temperature

The usual approach, as LibreHardwareMonitor and similar tools take it, uses two model-specific
registers (MSRs). Check them against the Intel 64 and IA-32 Architectures Software Developer's Manual,
volume 4 (MSRs) and volume 3 (thermal monitoring), for the models you target:

| MSR | Address | Bits | Meaning |
|---|---|---|---|
| `MSR_TEMPERATURE_TARGET` | `0x1A2` | 23:16 | TjMax, the temperature target, in °C |
| `IA32_PACKAGE_THERM_STATUS` | `0x1B1` | 22:16 | package digital readout: how many °C **below** TjMax the package is |

```text
tjmax       = (MSR 0x1A2 >> 16) & 0xFF
readout     = (MSR 0x1B1 >> 16) & 0x7F
package °C  = tjmax - readout
```

Points to respect:

- **Availability.** `IA32_PACKAGE_THERM_STATUS` exists when CPUID leaf 6 reports package thermal
  management (`CPUID.06H:EAX` bit 6). Without it, return `UnsupportedCpu`.
- **TjMax.** Read `MSR_TEMPERATURE_TARGET` once in `open`. A TjMax of 0, or one outside a sane range
  (about 70 to 125 °C), means the value is not usable: fail rather than guess. Some older models do not
  implement that MSR; tools fall back to a per-model table, which is a guess to document if you add it.
- **Per-core readings.** `IA32_THERM_STATUS` (`0x19C`, bits 22:16, bit 31 = reading valid) gives one
  core. MSRs are read on the logical processor the call runs on, so a per-core reading needs the thread
  pinned to that core. The package MSR gives the same value on any core of the package, which is why
  the package temperature is the simpler first source.
- **Read only.** A source never writes an MSR.

### With PawnIO's IntelMSR module

PawnIO.Modules (<https://github.com/namazso/PawnIO.Modules>) provides `IntelMSR.bin`, built from
`IntelMSR.p`. It is written for Intel CPUs and exposes an ioctl that reads one MSR: the index goes
in, the 64-bit value comes out. Take the exact function name, argument layout and list
of allowed MSRs from `IntelMSR.p` of the release you vendor; do not assume them.

The PawnIO plumbing is the same as the AMD source's: load `PawnIOLib.dll` from PawnIO's install folder
(`pawnio_dll_path()`) with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, then
`pawnio_open`, `pawnio_load` with the module bytes, and `pawnio_execute` for each read; `pawnio_close`
and `FreeLibrary` in the destructor. Follow `PawnIoSource::open` step by step, including its checks on
the module size and its status codes. If you find yourself copying it whole, moving the shared part to
a helper in `src/sensors/` is welcome in the same pull request.

Keep the register decoding in a pure function, like `tctl_from_register` for AMD:

```cpp
// Package temperature in °C from the raw MSR values, or nullopt when TjMax is not usable.
std::optional<double> intel_package_celsius(std::uint64_t temperature_target, std::uint64_t package_therm_status);
```

## The module and its licence

`scripts/package.ps1` ships **every** `*.bin` of `third_party/pawnio-modules`, and `install.ps1` copies
every module of the distribution folder next to `lite-deck-sensors.exe`. Nothing in the scripts names a
module. For each module, `package.ps1` requires:

- **The signed binary, unmodified**, from a PawnIO.Modules release: `third_party/pawnio-modules/IntelMSR.bin`.
- **An entry in `SOURCE.txt`**, in the same form as the AMD one: a first line `IntelMSR.bin - ...`, the
  release, its URL, the licence and module source, and a line `SHA-256: <64 hex digits>`. The package
  stops when a module has no entry or when the file does not match its hash.
- **Its licence.** `IntelMSR.COPYING` when the module has its own licence text, else the shared
  `COPYING` (PawnIO.Modules is LGPL-2.1, so the shared one fits). It ships as `IntelMSR.COPYING.txt`.

Then add the module to `THIRD_PARTY_NOTICES.md` like `AMDFamily17.bin`, and to the README's licence
paragraph.

## Testing without the hardware

The tests never load PawnIO or the driver.

- **The decoding.** Unit-test the pure function with register values: a few readouts and TjMax values,
  TjMax 0, and the bits around the fields (bits 15 and 23 of `0x1B1` set must not change the result).
  `tests/test_tctl.cpp` does the same for AMD.
- **The selection.** `tests/test_temp_sources.cpp` checks the built-in registry and the selection for
  AMD and Intel identities. Once your entry is registered, update its Intel case: `GenuineIntel`
  family 6 must now pick your id, and AMD family 17h to 1Ah must still pick `amd-family17`.
- **The service with a fake.** The test *a source registered next to the built-in ones is picked and
  published without other changes* registers a fake Intel source next to the built-in ones, runs it
  through `choose_temp_source`, `temp_source_opener` and the `Publisher`, and reads the value back from
  a `SensorBlock`. Copy it for any service-level behaviour you need to show.
- **The open checks.** Call your `open` with a `CpuIdent` you build and a module path that does not
  exist: a CPU you refuse must give `UnsupportedCpu`, a supported one `ModuleMissing`, both before
  PawnIO is touched (see `tests/test_pawnio_source.cpp`).
- **A manual test.** Add one marked `* doctest::skip()` that opens the real source and prints the
  value, like *manual: PawnIoSource reads a plausible temperature*. It needs an administrator and
  PawnIO, and runs only with `--no-skip`.
- **Run the suite.** `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Test`, and
  `scripts/test-install-lib.ps1` if you touched the scripts.

## Checking with the real hardware

A new source must be verified on a real CPU of the family before it is merged; the tests prove the
wiring, not the registers.

- `lite-deck-sensors.exe --console`, from an administrator prompt, prints the status and the value
  every second (it publishes in a `Local\` section when the service already owns the global one).
- The log of the service (`logs\sensors.log` next to the exe) names your source at startup.
- The value follows a known tool (HWiNFO, LibreHardwareMonitor or the vendor's utility) within a degree
  or two, at idle and under a full load, for several minutes.
- Stopping and starting the service, and a reboot, bring the value back.
- On an AMD machine nothing changes: the log still names `amd-family17` and `AMDFamily17.bin`.
