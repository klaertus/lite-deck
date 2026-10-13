"""Minimal Win32 HID access via ctypes (no dependencies). Throwaway prototype."""
import ctypes as C
from ctypes import wintypes as W

setupapi = C.WinDLL("setupapi", use_last_error=True)
hid = C.WinDLL("hid", use_last_error=True)
k32 = C.WinDLL("kernel32", use_last_error=True)


class GUID(C.Structure):
    _fields_ = [("d1", W.DWORD), ("d2", W.WORD), ("d3", W.WORD), ("d4", C.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(C.Structure):
    _fields_ = [("cbSize", W.DWORD), ("guid", GUID), ("flags", W.DWORD), ("reserved", C.c_void_p)]


class HIDD_ATTRIBUTES(C.Structure):
    _fields_ = [("Size", W.ULONG), ("VendorID", W.USHORT), ("ProductID", W.USHORT), ("Version", W.USHORT)]


class HIDP_CAPS(C.Structure):
    _fields_ = [("Usage", W.USHORT), ("UsagePage", W.USHORT),
                ("InputReportByteLength", W.USHORT), ("OutputReportByteLength", W.USHORT),
                ("FeatureReportByteLength", W.USHORT), ("Reserved", W.USHORT * 17),
                ("rest", W.USHORT * 10)]


DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_RW = 0x3
OPEN_EXISTING = 3
INVALID_HANDLE = C.c_void_p(-1).value

setupapi.SetupDiGetClassDevsW.restype = C.c_void_p
setupapi.SetupDiGetClassDevsW.argtypes = [C.POINTER(GUID), W.LPCWSTR, W.HWND, W.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(GUID), W.DWORD,
                                                 C.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [C.c_void_p, C.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                      C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [C.c_void_p]
k32.CreateFileW.restype = C.c_void_p
k32.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, C.c_void_p]
k32.CloseHandle.argtypes = [C.c_void_p]
k32.WriteFile.argtypes = [C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
k32.ReadFile.argtypes = [C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
hid.HidD_GetHidGuid.argtypes = [C.POINTER(GUID)]
hid.HidD_GetAttributes.argtypes = [C.c_void_p, C.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [C.c_void_p, C.POINTER(C.c_void_p)]
hid.HidD_FreePreparsedData.argtypes = [C.c_void_p]
hid.HidP_GetCaps.argtypes = [C.c_void_p, C.POINTER(HIDP_CAPS)]
hid.HidD_SetOutputReport.argtypes = [C.c_void_p, C.c_void_p, W.ULONG]


def enumerate_paths():
    g = GUID()
    hid.HidD_GetHidGuid(C.byref(g))
    info = setupapi.SetupDiGetClassDevsW(C.byref(g), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    paths, i = [], 0
    try:
        while True:
            d = SP_DEVICE_INTERFACE_DATA()
            d.cbSize = C.sizeof(d)
            if not setupapi.SetupDiEnumDeviceInterfaces(info, None, C.byref(g), i, C.byref(d)):
                break
            i += 1
            need = W.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(info, C.byref(d), None, 0, C.byref(need), None)
            buf = C.create_string_buffer(need.value)
            C.cast(buf, C.POINTER(W.DWORD))[0] = 8  # cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA_W on x64
            if setupapi.SetupDiGetDeviceInterfaceDetailW(info, C.byref(d), buf, need, None, None):
                paths.append(C.wstring_at(C.addressof(buf) + 4))
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)
    return paths


def open_path(path, access=GENERIC_READ | GENERIC_WRITE):
    h = k32.CreateFileW(path, access, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)
    return None if h in (None, INVALID_HANDLE) else h


def caps(h):
    a = HIDD_ATTRIBUTES()
    a.Size = C.sizeof(a)
    hid.HidD_GetAttributes(h, C.byref(a))
    pp = C.c_void_p()
    c = HIDP_CAPS()
    if hid.HidD_GetPreparsedData(h, C.byref(pp)):
        hid.HidP_GetCaps(pp, C.byref(c))
        hid.HidD_FreePreparsedData(pp)
    return a, c


def find(vid, pid, usage_page=None):
    """Return list of (path, attrs, caps) for matching interfaces."""
    out = []
    for p in enumerate_paths():
        h = open_path(p, 0)
        if not h:
            continue
        try:
            a, c = caps(h)
        finally:
            k32.CloseHandle(h)
        if a.VendorID == vid and a.ProductID == pid and (usage_page is None or c.UsagePage == usage_page):
            out.append((p, a, c))
    return out


def write(h, data):
    n = W.DWORD()
    buf = C.create_string_buffer(bytes(data), len(data))
    if not k32.WriteFile(h, buf, len(data), C.byref(n), None):
        raise OSError(C.get_last_error(), "WriteFile failed")
    return n.value


def read(h, size):
    """Blocking read of one input report."""
    n = W.DWORD()
    buf = C.create_string_buffer(size)
    if not k32.ReadFile(h, buf, size, C.byref(n), None):
        raise OSError(C.get_last_error(), "ReadFile failed")
    return buf.raw[:n.value]
