"""Knob LED ring test. StreamDock must be closed. Each step is printed with a timestamp."""
import colorsys
import threading
import time

import n4hid
from probe import Device, heartbeat


def wait_key(dev):
    """Block until any LCD key is pressed."""
    while True:
        r = n4hid.read(dev.rh, dev.in_len)
        if r[1:4] == b"ACK" and r[6:8] == b"OK" and 0x01 <= r[10] <= 0x0A and r[11] == 1:
            return


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def main():
    dev = Device()
    threading.Thread(target=heartbeat, args=(dev,), daemon=True).start()
    dev.cmd(b"DIS")

    def colors(*rgb):  # four (r, g, b) tuples, knobs left -> right
        dev.cmd(b"SETLB" + bytes(c for t in rgb for c in t))

    def bright(p):
        dev.cmd(b"LBLIG" + bytes([p]))

    try:
        dev.cmd(b"CONNECT")
        bright(100)  # LBLIG must precede SETLB, otherwise the built-in rainbow keeps running
        colors(*[(0, 0, 0)] * 4)
        log("waiting for a key press")
        wait_key(dev)
        steps = [
            ("1 all off", lambda: colors(*[(0, 0, 0)] * 4), 4),
            ("2 all red", lambda: colors(*[(255, 0, 0)] * 4), 4),
            ("3 all green", lambda: colors(*[(0, 255, 0)] * 4), 4),
            ("4 all blue", lambda: colors(*[(0, 0, 255)] * 4), 4),
            ("5 red / green / blue / white", lambda: colors((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)), 6),
        ]
        for name, fn, secs in steps:
            fn()
            log(name)
            time.sleep(secs)

        log("6 white, brightness 100 -> 0 -> 100")
        colors(*[(255, 255, 255)] * 4)
        for p in list(range(100, -1, -10)) + list(range(0, 101, 10)):
            bright(p)
            time.sleep(0.3)

        log("7 rainbow animation, 30 updates/s for 8 s")
        t0 = time.time()
        n = 0
        while time.time() - t0 < 8:
            h = (time.time() - t0) / 4
            rgb = [tuple(int(v * 255) for v in colorsys.hsv_to_rgb((h + k / 4) % 1, 1, 1)) for k in range(4)]
            colors(*rgb)
            n += 1
            time.sleep(1 / 30)
        log(f"  sent {n} updates")

        log("8 DELED (built-in effect)")
        dev.cmd(b"DELED")
        time.sleep(5)
    finally:
        dev.cmd(b"CLE\0\0DC")
        dev.cmd(b"HAN")
        log("done")


if __name__ == "__main__":
    main()
