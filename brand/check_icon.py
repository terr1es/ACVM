import ctypes
import ctypes.wintypes as wt
import os
import sys

import numpy as np
from PIL import Image

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)

SHGFI_ICON = 0x100
SHGFI_LARGEICON = 0x0
SHGFI_SMALLICON = 0x1
DI_NORMAL = 0x3


class SHFILEINFO(ctypes.Structure):
    _fields_ = [
        ("hIcon", wt.HICON),
        ("iIcon", ctypes.c_int),
        ("dwAttributes", wt.DWORD),
        ("szDisplayName", wt.WCHAR * 260),
        ("szTypeName", wt.WCHAR * 80),
    ]


class BMIH(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
        ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


class BMI(ctypes.Structure):
    _fields_ = [("bmiHeader", BMIH), ("bmiColors", wt.DWORD * 3)]


shell32.SHGetFileInfoW.argtypes = [wt.LPCWSTR, wt.DWORD, ctypes.POINTER(SHFILEINFO),
                                   wt.UINT, wt.UINT]
shell32.SHGetFileInfoW.restype = ctypes.c_size_t
user32.GetDC.argtypes = [wt.HWND]
user32.GetDC.restype = wt.HDC
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.DrawIconEx.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, wt.HICON,
                              ctypes.c_int, ctypes.c_int, wt.UINT, wt.HBRUSH, wt.UINT]
user32.DrawIconEx.restype = wt.BOOL
user32.DestroyIcon.argtypes = [wt.HICON]
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateDIBSection.argtypes = [wt.HDC, ctypes.POINTER(BMI), wt.UINT,
                                   ctypes.POINTER(ctypes.c_void_p), wt.HANDLE, wt.DWORD]
gdi32.CreateDIBSection.restype = wt.HBITMAP
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]


def grab(path, size, small, out):
    flags = SHGFI_ICON | (SHGFI_SMALLICON if small else SHGFI_LARGEICON)
    sfi = SHFILEINFO()
    ret = shell32.SHGetFileInfoW(path, 0, ctypes.byref(sfi), ctypes.sizeof(sfi), flags)
    if not ret or not sfi.hIcon:
        print("  FAIL ret=%s err=%d" % (ret, ctypes.get_last_error()))
        return None

    hdc_screen = user32.GetDC(None)
    hdc = gdi32.CreateCompatibleDC(hdc_screen)
    bmi = BMI()
    bmi.bmiHeader.biSize = ctypes.sizeof(BMIH)
    bmi.bmiHeader.biWidth = size
    bmi.bmiHeader.biHeight = -size
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bits = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(hdc, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    gdi32.SelectObject(hdc, hbmp)
    ok = user32.DrawIconEx(hdc, 0, 0, sfi.hIcon, size, size, 0, None, DI_NORMAL)
    raw = ctypes.string_at(bits.value, size * size * 4)
    bgra = np.frombuffer(raw, dtype=np.uint8).reshape(size, size, 4).copy()
    rgba = bgra.copy()
    rgba[..., 0] = bgra[..., 2]
    rgba[..., 2] = bgra[..., 0]
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc)
    user32.ReleaseDC(None, hdc_screen)
    user32.DestroyIcon(sfi.hIcon)

    Image.fromarray(rgba, "RGBA").save(out)
    al = rgba[..., 3]
    sel = rgba[al > 200][:, :3]
    if len(sel):
        cols, cnt = np.unique(sel, axis=0, return_counts=True)
        order = np.argsort(-cnt)[:3]
        top = ", ".join("#%02X%02X%02X x%d" % (cols[i][0], cols[i][1], cols[i][2], cnt[i])
                        for i in order)
    else:
        top = "(fully transparent)"
    print("  %2dpx ok  mean alpha %.0f  opaque %.0f%%  dominant %s -> %s" % (
        size, al.mean(), 100 * (al > 200).mean(), top, out))
    return rgba


AX = r"C:\Users\tb918\Desktop\Axiom\examples\hello.ax"
ICO = r"C:\Users\tb918\.local\share\axiom\axiom.ico"
T = r"C:\Users\tb918\AppData\Local\Temp"



SHCNE_ASSOCCHANGED = 0x08000000
SHCNF_IDLIST = 0x0000
try:
    shell32.SHChangeNotify.argtypes = [ctypes.c_long, wt.UINT, ctypes.c_void_p, ctypes.c_void_p]
    shell32.SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, None, None)
    print("broadcast SHCNE_ASSOCCHANGED")
except Exception as exc:  # noqa: BLE001
    print("broadcast failed:", exc)

if len(sys.argv) > 1:
    # Git Bash rewrites "C:\a\b" into "C:/a/b" (or "C://a//b"); normpath repairs it.
    targets = [os.path.normpath(a) for a in sys.argv[1:]]
else:
    targets = [os.path.normpath(AX)]

for i, path in enumerate(targets):
    print("\n%s:" % path)
    grab(path, 32, False, T + r"\check32_%d.png" % i)
    grab(path, 16, True, T + r"\check16_%d.png" % i)

if len(sys.argv) == 1:
    print("\naxiom.ico used as the icon for the file itself (for comparison):")
    grab(ICO, 32, False, T + r"\self32.png")
