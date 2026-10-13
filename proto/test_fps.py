"""Strip marquee frame-rate test via BGPIC. StreamDock must be closed.

Scroll speed on screen is constant (120 px/s); only the frame rate changes:
10, 20, 30 fps, then as fast as the device accepts. Current phase is shown on the top-left key.
"""
import glob
import os
import threading
import time

from probe import Device, IMG, heartbeat
from test_leds import wait_key

SPEED = 120   # px/s
PX_PER_FILE = 2
PHASE = 8     # seconds


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def main():
    frames = [open(p, "rb").read() for p in sorted(glob.glob(os.path.join(IMG, "frames", "f*.jpg")))]
    dev = Device()
    threading.Thread(target=heartbeat, args=(dev,), daemon=True).start()
    dev.cmd(b"DIS")
    dev.cmd(b"LIG\0\0" + bytes([60]))
    dev.cmd(b"BGCLE" + bytes([3]))
    dev.cmd(b"CLE\0\0\0" + bytes([0xFF]))
    dev.cmd(b"STP")

    def label(name):
        with open(os.path.join(IMG, "frames", f"label_{name}.jpg"), "rb") as f:
            dev.key_image(11, f.read())
        dev.cmd(b"STP")

    try:
        log(f"{len(frames)} frames loaded, waiting for a key press")
        wait_key(dev)
        for fps in (10, 20, 30, None):
            label(str(fps) if fps else "MAX")
            step = round(SPEED / fps / PX_PER_FILE) if fps else 1
            interval = 1 / fps if fps else 0
            pos, sent, write_time = 0.0, 0, 0.0
            t0 = time.perf_counter()
            next_t = t0
            while time.perf_counter() - t0 < PHASE:
                w0 = time.perf_counter()
                dev.bgpic(frames[int(pos) % len(frames)], 0, 0, 800, 112)
                write_time += time.perf_counter() - w0
                sent += 1
                pos += step
                next_t += interval
                delay = next_t - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
            elapsed = time.perf_counter() - t0
            log(f"target {fps or 'max'} fps: {sent / elapsed:.1f} fps achieved, "
                f"{write_time / sent * 1000:.1f} ms per frame write")
    finally:
        dev.cmd(b"BGCLE" + bytes([3]))
        dev.cmd(b"CLE\0\0DC")
        dev.cmd(b"HAN")
        log("done")


if __name__ == "__main__":
    main()
