"""List every HID interface of the N4 Pro with its report sizes. Safe while StreamDock runs."""
from n4hid import find

for path, a, c in find(0x5548, 0x1021):
    print(f"{path}\n  ver=0x{a.Version:04x} usagePage=0x{c.UsagePage:04x} usage=0x{c.Usage:04x} "
          f"in={c.InputReportByteLength} out={c.OutputReportByteLength} feat={c.FeatureReportByteLength}")
