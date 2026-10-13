# MiraBox N4 Pro E (VID 0x5548, PID 0x1021) - observed protocol

Measured on 2026-10-11 with `probe.py` (Win32 `ReadFile`/`WriteFile`, so byte 0 is the report ID 0x00).
Sources for the command set: MiraBox StreamDock-Device-SDK, mirajazz, ambiso/opendeck-akp05.

## Interface
- Vendor HID interface `MI_00`, usage page 0xFFA0: input 513 bytes, output 1025 bytes.
- Use separate handles for reading and writing (a synchronous handle serializes I/O).
- Killing StreamDock makes the device re-enumerate for a few seconds: opening during that window fails with error 1167.

## Output (buf[0]=0x00, buf[1..5]="CRT\0\0", zero-padded to 1025)
| Cmd | Body from buf[6] |
|---|---|
| DIS | `DIS` wake |
| LIG | `LIG\0\0` + brightness 0-100 |
| CLE | `CLE\0\0\0` + index (0xFF = all) |
| STP | `STP` commit after image uploads |
| CONNECT | heartbeat every 10 s |
| CLE DC / HAN | disconnect / screen off |
| BAT | `BAT` + u32 BE JPEG length + index, then raw 1024-byte chunks |

Image indices (verified visually): top keys 11-15, bottom keys 6-10, strip zones 1-4, left→right.
Keys 112×112, zones 176×112, JPEG baseline rotated 180° (displays upright).

## Input (`ACK` at buf[1..3])
`OK` at buf[6..7]; code at buf[10], state at buf[11].

| Code | Meaning (verified) |
|---|---|
| 0x01-0x05 / 0x06-0x0A | top / bottom keys; state 1 = down, 0 = up |
| 0xA0/0xA1, 0x50/0x51, 0x90/0x91, 0x70/0x71 | knobs 1-4, left (CCW) / right (CW); one report per detent |
| 0x37, 0x35, 0x33, 0x36 | knobs 1-4 press; press only, no release report |
| 0x00, state 1 | strip **finger down**; X at buf[12..13] (u16 BE) |
| 0x40-0x43 | strip tap ended in zone 1-4 (finger up); X at buf[12..13] |
| 0x38 / 0x39 | swipe left→right / right→left (finger up) |

`ARX` at buf[5..7]: touch position stream while the finger is down, ~30-40 reports/s,
repeated even when stationary. X = buf[11..12] (0-800), Y = buf[13..14] (≈15-90), u16 BE.
→ Full gesture tracking is possible: down / move / up, long press, drag, arbitrary hit-testing.

## Display model (verified 2026-10-11 with `test_strip.py`)
The whole face is **one 800×480 panel mounted upside down**; key images are drawn on a layer above a background layer.
- `BGPIC` + u32 BE length + x, y, w, h (u16 BE) + 0 + layer, then 1024-byte chunks, no STP:
  draws a JPEG (rotated 180°) at any rectangle of the background layer.
- Device coordinates start at the **bottom-right** corner as seen by the user:
  `x_dev = 800 - x_user - w`, `y_dev = 480 - y_user - h`.
- Full strip, upright and full width: BGPIC 800×112 at device (0, 0). At device y=368 the image lands behind the top row of keys.
- BGPIC 800×480 at (0, 0) fills the whole face; key images stay on top, and the background shows around their edges.
- BAT index 1 with an 800-px image is squeezed into zone 1 (176 px): zones cannot go full width.
- `BGCLE` + 3 clears the background layer.

## Knob LED rings (verified 2026-10-11 with `test_leds.py`)
- `LBLIG` + brightness (W11): ring brightness, 0-100 verified (SDK hints up to 255).
- `SETLB` + 4 × (R, G, B) from W11, knobs left→right, no count byte, no STP: static colour per ring.
- `DELED`: back to the built-in rainbow effect.
- 30 SETLB updates/s were accepted and animate smoothly (236 in 8 s), so the host can drive its own effects.
- **Quirk:** SETLB with all four rings black (0,0,0) is ignored and the built-in rainbow stays or comes back.
  Black on some rings works as long as at least one ring has a colour (verified with `test_led_off.py`).
- Turn all rings off with `LBLIG 0` (verified). Near-black (1,1,1) stays visibly lit, so don't use it.
- QUCMD LedFollowKeyLight is not needed for any of this.

## Strip frame rate (verified 2026-10-11 with `test_fps.py`)
BGPIC 800×112, ~12 KB JPEG (q80) per frame, marquee at 120 px/s:
- 10 fps readable but jerky, 20 fps so-so, **30 fps smooth**.
- One frame write takes ~2 ms on the host (WriteFile returns before display), so 30 fps costs ~6% of the frame budget.
- Unpaced (~600 writes/s) the display **freezes**: the device accepts data without back-pressure and drops it.
  The host must pace frames itself (≤ 30 fps is safe; 45-60 untested).

## Not yet tested
- Vibration (QUCMD), behaviour without heartbeat, frame rates between 30 and 600.
