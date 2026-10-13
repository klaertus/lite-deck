"""Which command turns the knob rings off? Phase N is shown as "ZONE N" on the strip. StreamDock must be closed.

Each phase first sets all rings red (to leave rainbow mode), then tries one way to switch off:
1. SETLB (1, 1, 1)           near-black instead of exact black
2. LBLIG 0                   ring brightness 0
3. SETLB (0, 0, 0) per ring  but ring 1 red, to see if black is ignored only when all four are black
4. QUCMD LedFollowKeyLight=Off then LBLIG 0
"""
import threading
import time

from probe import Device, heartbeat, read_img
from test_leds import wait_key

RED = (255, 0, 0)


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def main():
    dev = Device()
    threading.Thread(target=heartbeat, args=(dev,), daemon=True).start()
    dev.cmd(b"DIS")
    dev.cmd(b"LIG\0\0" + bytes([60]))
    dev.cmd(b"CLE\0\0\0" + bytes([0xFF]))
    dev.cmd(b"STP")

    def colors(*rgb):
        dev.cmd(b"SETLB" + bytes(c for t in rgb for c in t))

    def bright(p):
        dev.cmd(b"LBLIG" + bytes([p]))

    def show_phase(n):
        dev.cmd(b"CLE\0\0\0" + bytes([0xFF]))
        dev.key_image(n, read_img(f"zone{n}.jpg"))
        dev.cmd(b"STP")

    phases = [
        lambda: colors(*[(1, 1, 1)] * 4),
        lambda: bright(0),
        lambda: colors(RED, (0, 0, 0), (0, 0, 0), (0, 0, 0)),
        lambda: (dev.cmd(b"QUCMD" + bytes([0xFF, 0x11, 0x11, 0x11, 0x11, 0x11])), bright(0)),
    ]
    try:
        log("waiting for a key press")
        wait_key(dev)
        for n, attempt in enumerate(phases, 1):
            show_phase(n)
            bright(100)
            colors(*[RED] * 4)
            log(f"phase {n}: red for 3 s")
            time.sleep(3)
            attempt()
            log(f"phase {n}: off attempt for 5 s")
            time.sleep(5)
        dev.cmd(b"QUCMD" + bytes([0x1F, 0x11, 0x00, 0x11, 0x00, 0x11]))  # official app's values
        dev.cmd(b"DELED")
        log("restored")
    finally:
        dev.cmd(b"CLE\0\0DC")
        dev.cmd(b"HAN")
        log("done")


if __name__ == "__main__":
    main()
