"""N4 Pro E protocol probe: draws test images and logs every input report.

StreamDock must be closed. Usage:
  py probe.py [--strip zones|full|bgpic|face] [--seconds N]
Events go to the console and to events.log.
"""
import argparse
import os
import struct
import threading
import time

import n4hid

VID, PID, USAGE_PAGE = 0x5548, 0x1021, 0xFFA0
HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "img")

KEYS = {**{0x01 + i: f"key T{i + 1}" for i in range(5)}, **{0x06 + i: f"key B{i + 1}" for i in range(5)}}
KNOBS = {0xA0: "knob1 CCW", 0xA1: "knob1 CW", 0x50: "knob2 CCW", 0x51: "knob2 CW",
         0x90: "knob3 CCW", 0x91: "knob3 CW", 0x70: "knob4 CCW", 0x71: "knob4 CW",
         0x37: "knob1 press", 0x35: "knob2 press", 0x33: "knob3 press", 0x36: "knob4 press",
         0x40: "strip zone1", 0x41: "strip zone2", 0x42: "strip zone3", 0x43: "strip zone4",
         0x38: "swipe 0x38", 0x39: "swipe 0x39", 0x00: "code 0x00"}


class Device:
    def __init__(self):
        found = n4hid.find(VID, PID, USAGE_PAGE)
        if not found:
            raise SystemExit("N4 Pro vendor interface not found")
        path, _, caps = found[0]
        self.out_len = caps.OutputReportByteLength  # 1025
        self.in_len = caps.InputReportByteLength    # 513
        # Two handles: a synchronous handle serializes I/O, so a blocking read would stall writes.
        self.h = n4hid.open_path(path)
        self.rh = n4hid.open_path(path)
        if not self.h or not self.rh:
            raise SystemExit("cannot open device (is StreamDock still running?)")
        self.lock = threading.Lock()

    def send(self, payload: bytes):
        buf = bytes([0]) + payload
        buf = buf + bytes(self.out_len - len(buf))
        with self.lock:
            n4hid.write(self.h, buf)

    def cmd(self, body: bytes):
        self.send(b"CRT\0\0" + body)

    def upload(self, header: bytes, data: bytes):
        """Header command then raw 1024-byte chunks, all under one lock so the heartbeat can't interleave."""
        chunk = self.out_len - 1
        with self.lock:
            for part in [b"CRT\0\0" + header] + [data[i:i + chunk] for i in range(0, len(data), chunk)]:
                buf = bytes([0]) + part
                n4hid.write(self.h, buf + bytes(self.out_len - len(buf)))

    def key_image(self, index: int, jpeg: bytes):
        self.upload(b"BAT" + struct.pack(">IB", len(jpeg), index), jpeg)

    def bgpic(self, jpeg: bytes, x, y, w, h, layer=0):
        self.upload(b"BGPIC" + struct.pack(">IHHHHBB", len(jpeg), x, y, w, h, 0, layer), jpeg)


def read_img(name):
    with open(os.path.join(IMG, name), "rb") as f:
        return f.read()


def describe(r: bytes) -> str:
    if len(r) < 12:
        return "short"
    if r[1:4] == b"ACK" and r[5:8] == b"ARX":
        return f"TOUCH x={r[11] << 8 | r[12]} y={r[13] << 8 | r[14]}"
    if r[1:4] == b"ACK" and r[6:8] == b"OK":
        code, state = r[10], r[11]
        if code == 0xFF:
            return "write ack"
        if code in KEYS:
            return f"{KEYS[code]} {'down' if state else 'up'}"
        return f"{KNOBS.get(code, f'UNKNOWN 0x{code:02x}')} state={state}"
    return "unrecognised"


def reader(dev, log):
    while True:
        try:
            r = n4hid.read(dev.rh, dev.in_len)
        except OSError as e:
            print("read error:", e)
            return
        line = f"{time.strftime('%H:%M:%S')} {describe(r):28s} | {r[:24].hex(' ')}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()


def heartbeat(dev):
    while True:
        time.sleep(10)
        dev.cmd(b"CONNECT")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strip", choices=["zones", "full", "bgpic", "face"], default="zones")
    ap.add_argument("--seconds", type=int, default=0, help="0 = run until Ctrl+C")
    a = ap.parse_args()

    dev = Device()
    log = open(os.path.join(HERE, "events.log"), "a", encoding="utf-8")
    log.write(f"--- session {time.strftime('%Y-%m-%d %H:%M:%S')} strip={a.strip}\n")
    threading.Thread(target=reader, args=(dev, log), daemon=True).start()

    dev.cmd(b"DIS")
    dev.cmd(b"LIG\0\0" + bytes([60]))
    dev.cmd(b"CLE\0\0\0" + bytes([0xFF]))
    dev.cmd(b"STP")

    for idx in list(range(6, 16)):
        dev.key_image(idx, read_img(f"key{idx}.jpg"))
    if a.strip == "zones":
        for z in range(1, 5):
            dev.key_image(z, read_img(f"zone{z}.jpg"))
    elif a.strip == "full":
        dev.key_image(1, read_img("strip800.jpg"))
    dev.cmd(b"STP")
    if a.strip == "bgpic":
        dev.bgpic(read_img("strip800.jpg"), 0, 368, 800, 112)
    elif a.strip == "face":
        dev.bgpic(read_img("face800x480.jpg"), 0, 0, 800, 480)
    print(f"images sent (strip={a.strip}); press keys, turn/press knobs, tap and swipe the strip")

    threading.Thread(target=heartbeat, args=(dev,), daemon=True).start()
    try:
        end = time.time() + a.seconds if a.seconds else None
        while end is None or time.time() < end:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        dev.cmd(b"CLE\0\0DC")
        dev.cmd(b"HAN")
        log.close()
        print("done")


if __name__ == "__main__":
    main()
