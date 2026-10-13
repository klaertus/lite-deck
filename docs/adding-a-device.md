# Adding a device

lite-deck drives the **MiraBox N4 Pro E**, and that is the only device tested on real hardware. The
device layer is built so that someone who owns another stream controller can add it without touching
the core: a driver in its own folder and one line in the device registry. The configuration, the
deck, the key layout, the web page and the simulator all follow the driver's data sheet.

This guide covers the contract in `src/device/device.h`, the registry, how to work out a protocol,
how to test without the hardware, and what to check with it.

## How devices are built

**Drivers are compiled into the daemon**, like modules: no plugin DLL, no stable ABI. One device model
is active at a time. It is **chosen once, at startup**: the first registered device that is plugged in,
else the one named by `device.model` in `config.json`, else the N4 Pro. It stays active until the
daemon restarts: the daemon only reconnects that model, and another model plugged in later is picked
up at the next start.

| Step | File |
|---|---|
| Write the driver | `src/device/<model>/<model>_device.h` and `.cpp`, plus any protocol helpers in the same folder. CMake picks up every `src/**/*.cpp`, so `CMakeLists.txt` does not change. |
| Register it | Add `#include "device/<model>/<model>_device.h"` and `<model>::entry(),` to `device_registry()` in `src/device/registry.cpp`. |
| Test it | `tests/test_<model>.cpp`, with a fake transport (see [Testing](#testing-without-the-device)). |

Nothing else changes. The N4 Pro driver in `src/device/n4pro/` is the reference implementation. Put
everything of your driver, protocol helpers included, in `namespace ld::<model>` (the N4 Pro uses
`ld::n4pro`): drivers are linked together, so a copied helper in another namespace would clash.

## The contract (`src/device/device.h`)

### `DeviceLayout`: the data sheet

```cpp
struct DeviceLayout {
  std::string name;                // shown to the user, e.g. "MiraBox N4 Pro E"
  std::string id;                  // stable, written in config.json (device.model): never rename it
  UsbMatch usb;                    // vid, pid, optional usage_page, output_report size
  int rows = 0, cols = 0;          // keys, numbered left to right, then top to bottom
  PixelSize key_size;              // px; the key template scales by height / 112
  std::optional<PixelSize> strip;  // the touch strip, if any
  int knobs = 0;
  bool knob_rings = false;         // RGB rings around the knobs
  Duration heartbeat{10000};       // how often heartbeat() is called while connected
  std::string vendor_app;          // the vendor app that takes the device, e.g. "StreamDock.exe" ("" = none)
};
```

`UsbMatch` selects the HID interface to open: its VID and PID, its usage page when set, and the size
of its output report, report id included, when `output_report` is not 0.

**Every write must be exactly one output report of the interface**, report id included: Windows
refuses a HID write of any other size, and a refused write makes the manager reconnect. Set
`output_report` to that size (`probe_caps.py` gives it) and build every report at that size. With
`output_report = 0` any interface of the VID and PID matches whatever its report size, and the driver
cannot ask the `Transport` for it: use 0 only when the device has a single interface whose size your
driver already knows.

`heartbeat` is the keep-alive interval (the N4 Pro uses 10 s); a device that needs none can leave the
default and do nothing in `heartbeat()`. `vendor_app` names the executable of the vendor's own
software: while it runs, lite-deck releases the device and shows "<name> active: deck released"
("StreamDock" for the N4 Pro). Leave it empty when no such software competes for the device.

The registry checks every data sheet at startup (`layout_errors` and `sound_devices` in
`src/device/registry.cpp`). The limits keep the daemon within its memory budget:

| Field | Accepted |
|---|---|
| `id` | not empty, at most 64 characters, used by one driver only |
| `name` | not empty |
| keys | `rows` and `cols` at least 1, at most 64 keys in all |
| `key_size` | each side 1 to 512 px |
| `strip` | 60 to 4096 px wide (room for one zone), 1 to 512 px high |
| `knobs` | 0 to 16 |
| `heartbeat` | 100 ms to 60 s |
| `vendor_app` | at most 64 characters |
| factory | present |

A data sheet that fails is left out and the reason is logged; the other drivers still load.

Keys need not be square: the key engine takes the scale from the height and spreads the rows across
the width. Module sizes are declared at 112 px.

### `Device`: what the driver does

```cpp
class Device {
public:
  virtual void initialize(std::uint8_t brightness) = 0;
  virtual void set_brightness(std::uint8_t pct) = 0;
  virtual void heartbeat() = 0;      // every layout.heartbeat while connected
  virtual void sleep() = 0;          // session locked or PC asleep: drop queued frames, go dark
  virtual void shutdown() = 0;       // quit or end of session
  virtual void key_image(int slot, const Image& image) = 0;
  virtual void strip_image(const Image& image) = 0;
  virtual void set_rings(std::span<const Rgb> colors, std::uint8_t brightness) = 0;
  virtual DeviceInput decode_input(std::span<const std::uint8_t> report) = 0;
  virtual std::optional<TimePoint> next_flush(TimePoint now) const = 0;
  virtual void flush(TimePoint now) = 0;
  virtual bool failed() const = 0;
};
```

The `DeviceManager` (`src/device/device_manager.cpp`) owns the connection. Each time it opens the HID
interface that matches `usb`, it calls your factory with that link, a `Transport`
(`src/device/transport.h`) whose `write` sends one output report, report id first, of exactly the
interface's output report size. Then it calls
`initialize`, and:

- **Lifetime.** The `Transport` outlives the `Device` made on it: on every disconnect the manager
  destroys the driver first and closes the link after, so the driver may keep a reference to it. A
  new driver is made on every connection. A factory that returns `nullptr` counts as a failed
  connection: it is logged and retried later.
- **Frames.** `key_image` and `strip_image` hand you a raw frame (`Image`, `src/core/image.h`):
  premultiplied BGRA, the size of your key or strip, `stride` bytes per row. It is **lent for the call
  only**: encode or copy it before returning. For JPEG, keep a `JpegEncoder` member
  (`src/render/jpeg.h`); it reuses its buffers. If the screen is mounted turned or mirrored, pass the
  `Orientation` to `encode`: `Rotate90`, `Rotate180`, `Rotate270` (clockwise; 90 and 270 swap the
  encoded width and height), `FlipHorizontal` or `FlipVertical`. The N4 Pro passes `Rotate180`. You
  never need to edit `src/render/`.
- **Pacing.** Queue frames and send them from `flush`. The manager calls `next_flush(now)` after each
  frame and calls `flush(now)` at the time you return; return `nullopt` when nothing is queued. A driver
  that sends at once always returns `nullopt`.
- **Errors.** When a `write` returns false, stop writing and make `failed()` true: the manager drops the
  link and reconnects.
- **Input.** Each report read from the device goes to `decode_input`. Return `InputKind::None` for
  anything that is not an input (acknowledgements, unknown codes).
- **Threading.** Everything runs on the daemon's loop thread. Do not block: a HID write already waits
  up to one second.

### `DeviceInput`: what the device reports

| `kind` | Fields | Reaches |
|---|---|---|
| `KeyDown`, `KeyUp` | `index`: key slot | the key's module (press, release, tap, long press) |
| `KnobTurn` | `index`: knob, `delta`: notches turned | the knob's module (rotate) |
| `KnobPress`, `KnobRelease` | `index`: knob | the knob's module (press, release) |
| `TouchDown`, `TouchMove`, `TouchUp` | `x`, `y` in strip pixels (`TouchUp` may carry 0) | the strip gestures (tap, long touch, swipes) |

`delta` is the signed number of notches: negative counter-clockwise, positive clockwise. The N4 Pro
reports each notch on its own, so it always sends −1 or +1; a device that reports several notches at
once passes the count, and the knob modules step once per notch (`volume` and `mic` by their step,
`seek` by its seconds, `media` one track per notch). Leave `delta` at 0 for the other kinds.

Indexes outside the data sheet are ignored, and so is touch input on a device without a strip.

### Rings

`set_rings` gets one colour per knob, and only when `knob_rings` is true. The colours come from the
modules (a muted microphone turns red) and from the Device panel.

## Registering the driver

```cpp
// src/device/registry.cpp
const std::vector<DeviceEntry>& device_registry() {
  static const std::vector<DeviceEntry> registry = sound_devices({
      n4pro::entry(),
      acme::entry(),  // your driver
  });
  return registry;
}
```

Each driver exposes its data sheet and a `DeviceEntry` (the data sheet and a factory), like the N4 Pro:

```cpp
namespace ld::acme {

const DeviceLayout& layout() {
  static const DeviceLayout l{.name = "Acme Deck 6",
                              .id = "acme-deck6",
                              .usb = UsbMatch{.vid = 0x1234, .pid = 0x5678,
                                              .usage_page = std::uint16_t{0xFF00}, .output_report = 513},
                              .rows = 2,
                              .cols = 3,
                              .key_size = {96, 96},
                              .strip = std::nullopt,
                              .knobs = 0,
                              .knob_rings = false,
                              .heartbeat = Duration{10000},
                              .vendor_app = "AcmeHub.exe"};
  return l;
}

DeviceEntry entry() {
  return DeviceEntry{layout(), [](Transport& t) -> std::unique_ptr<Device> { return std::make_unique<AcmeDevice>(t); }};
}

}  // namespace ld::acme
```

(The Acme values are made up: take yours from the device.)

## The configuration

`config.json` follows the active device:

- `device.model` holds the id of the device the file goes with. It is written only when that device is
  not the N4 Pro, so an N4 Pro configuration keeps its exact content. When the active device differs
  from the one the file names, the daemon writes the new id once and logs it; an id this build does not
  know is logged and replaced.
- The `keys` and `knobs` lists grow to your device's counts with empty slots. **Slots beyond your
  device are kept** in the file, not shown and not started: a configuration written on a larger device
  survives a stay on a smaller one. The web UI sends them back unchanged; a module added or changed
  beyond the device is refused with a 422 error.
- On a device without a strip, the `strip` section is still checked and kept, but no zone starts.
- `device.ring_colors` grows to the number of knobs.

## Working out the protocol

- **`proto/`** holds what was learnt for the N4 Pro: `PROTOCOL.md` (commands, image upload, input
  codes, pacing), `n4hid.py` (Win32 HID access from Python, no dependency), `probe_caps.py` (every HID
  interface of a device with its report sizes), `probe.py` (draws test images and logs every input
  report) and a few single-purpose tests. Copy them, change the VID and PID, and start there.
- **Find the interface.** In Device Manager, the device's *Hardware Ids* give the VID and PID;
  `probe_caps.py` gives the usage page and the input and output report sizes of each interface.
- **Watch the vendor software.** With USBPcap and Wireshark, record what the vendor application sends
  when it changes an image, the brightness or a ring colour. Close the vendor application before
  lite-deck or a probe opens the device: both cannot hold it.
- **Reference projects.** The N4 Pro command set came from the MiraBox StreamDock-Device-SDK, from
  mirajazz (a Rust library for MiraBox and Ajazz devices) and from ambiso/opendeck-akp05 (a device
  plugin for the OpenDeck application). Many MiraBox and Ajazz models share that protocol with other
  sizes and codes.

## Testing without the device

- **Unit tests with a fake transport.** Give your driver a `Transport` that records what it writes and
  check the reports byte for byte (`RecordingTransport` in `tests/test_n4pro.cpp`). Feed
  `decode_input` the reports you captured.
- **The whole chain with a fake device.** `tests/fake_device.*` defines the *Test Deck* (3 × 5 keys of
  96 px, no strip, 2 knobs without rings) and its recording driver; `tests/test_device_profiles.cpp`
  runs it through the config, the deck, the `DeviceManager`, the web API and the simulator. The Test
  Deck is not in `device_registry()`: the tests pass its `DeviceEntry` where they need it. Use it as a
  model for your device.
- **The simulator.** `lite-deck.exe --simulate` renders for the active device and writes each frame to
  `%APPDATA%\lite-deck\sim`; its `inject.txt` commands (see the README) are bounded by your data sheet.
  With no supported device plugged in, set `"model": "<your id>"` in the `device` object of
  `config.json` to make yours the active one.
- **Run the suite.** `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Test`. The
  N4 Pro traffic is pinned by `tests/test_n4pro_pinned.cpp` and the rendering by the reference images
  in `tests/golden`: both must keep passing.

## Checking with the real hardware

- Every key shows its own frame, in the right place and the right way up.
- Keys, knobs and touch reach the module under your finger (`%APPDATA%\lite-deck\logs\lite-deck.log`).
- Brightness and ring colours follow the Device panel; locking the session darkens the device and
  unlocking redraws it.
- Unplugging and plugging the device back reconnects within a few seconds.
- A clock or a scrolling strip zone runs for several minutes without frames piling up, and the
  daemon's memory stays flat (Task Manager, *Memory* column).
- `config.json` now has your id in `device.model`; a config written on an N4 Pro loads, keeps the
  slots your device lacks, and shows empty slots where it has more.
