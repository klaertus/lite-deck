"""Full-width touch strip test, 4 labelled phases of PHASE seconds each. StreamDock must be closed.

A: BAT index 1 with one 800x112 image
B: BGPIC 800x112 at (0, 368)
C: BGPIC 800x112 at (0, 0)
D: BGPIC 800x480 at (0, 0) (official SDK usage)
"""
import threading
import time

from probe import Device, read_img, heartbeat

PHASE = 25


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def reset(dev):
    dev.cmd(b"BGCLE" + bytes([3]))
    dev.cmd(b"CLE\0\0\0" + bytes([0xFF]))
    dev.cmd(b"STP")
    for idx in range(6, 16):
        dev.key_image(idx, read_img(f"key{idx}.jpg"))
    dev.cmd(b"STP")


def main():
    dev = Device()
    threading.Thread(target=heartbeat, args=(dev,), daemon=True).start()
    dev.cmd(b"DIS")
    dev.cmd(b"LIG\0\0" + bytes([60]))

    phases = [
        ("A BAT idx1 800x112", lambda: (dev.key_image(1, read_img("stripA.jpg")), dev.cmd(b"STP"))),
        ("B BGPIC y=368", lambda: dev.bgpic(read_img("stripB.jpg"), 0, 368, 800, 112)),
        ("C BGPIC y=0", lambda: dev.bgpic(read_img("stripC.jpg"), 0, 0, 800, 112)),
        ("D BGPIC 800x480", lambda: dev.bgpic(read_img("faceD.jpg"), 0, 0, 800, 480)),
    ]
    try:
        for name, show in phases:
            reset(dev)
            show()
            log(f"phase {name}")
            time.sleep(PHASE)
    finally:
        dev.cmd(b"BGCLE" + bytes([3]))
        dev.cmd(b"CLE\0\0DC")
        dev.cmd(b"HAN")
        log("done")


if __name__ == "__main__":
    main()
