#!/usr/bin/env python3
"""GUI verification driver for ra9fael.envhosts.

Opens both editor windows through the plugin's real JSON-RPC protocol,
captures them with PrintWindow, writes PNG previews under tests/fixtures/,
then keeps the process alive so the windows stay on the desktop.

Preview-only data lives in the isolated HKCU\\Software\\WinShunTest\\EnvHosts
subkey and tests/fixtures/hosts.sample; the real Environment key and the real
hosts file are never written.
"""
import ctypes
import json
import os
import struct
import subprocess
import tempfile
import threading
import time
import winreg
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUTDIR = os.path.join(HERE, "fixtures")
EXE = os.path.join(ROOT, "envhosts.exe")
DATA = tempfile.mkdtemp(prefix="ws-envhosts-preview-")
SUBKEY = r"Software\WinShunTest\EnvHosts"

# A hosts preview with many real entries plus two group headings, so scrolling
# and both row kinds can be verified. Kept in the temp data dir, never in the
# real System32 hosts file.
HOSTS = os.path.join(DATA, "hosts.preview")
with open(HOSTS, "w", encoding="utf-8", newline="\r\n") as f:
    f.write("# sample hosts for GUI preview\n")
    for i in range(1, 25):
        f.write("10.%d.%d.%d host%02d.example.com host%02d  # row %02d\n"
                % (i // 10, i, i, i, i, i))
    f.write("# trailing group\n")
    f.write("127.0.0.1 localhost\n")

# Preview-only sample rows in the isolated test subkey. The real user
# Environment key and the real hosts file are never touched.
with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, SUBKEY, 0, winreg.KEY_SET_VALUE) as k:
    winreg.SetValueEx(k, "EPICS_CA_ADDR_LIST", 0, winreg.REG_SZ,
                      "10.10.81.255 192.168.138.255")
    winreg.SetValueEx(k, "LANG", 0, winreg.REG_SZ, "C")
    winreg.SetValueEx(k, "Path", 0, winreg.REG_EXPAND_SZ,
                      r"C:\Windows\System32;C:\Windows")
    winreg.SetValueEx(k, "WS_DEMO_NOTE", 0, winreg.REG_SZ, "sample value for GUI preview")
    # Enough rows to force scrolling: the table shows ~16 rows at default size.
    for i in range(1, 31):
        winreg.SetValueEx(k, "WS_ROW_%02d" % i, 0, winreg.REG_SZ,
                          "sample row %02d" % i)
    try:
        winreg.DeleteValue(k, "WS_DEMO_NUM")
    except FileNotFoundError:
        pass

env = dict(os.environ)
env["WS_DATA_DIR"] = DATA
env["WS_ENVHOSTS_TEST_SUBKEY"] = SUBKEY
env["WS_ENVHOSTS_TEST_HOSTS"] = HOSTS

proc = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, env=env, text=True, bufsize=1,
                        encoding="utf-8", errors="replace")
buf = []
lock = threading.Lock()


def pump():
    for line in proc.stdout:
        with lock:
            buf.append(line)


threading.Thread(target=pump, daemon=True).start()


def recv(timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            if buf:
                return buf.pop(0)
        time.sleep(0.01)
    return None


_id = 0


def call(method, params):
    global _id
    _id += 1
    mid = _id
    proc.stdin.write(json.dumps({"id": mid, "method": method, "params": params},
                                ensure_ascii=False) + "\n")
    proc.stdin.flush()
    return mid, recv()


def items_of(obj):
    if not isinstance(obj, dict):
        return []
    for key in ("result", "params", "data"):
        v = obj.get(key)
        if isinstance(v, dict) and isinstance(v.get("items"), list):
            return v["items"]
    if isinstance(obj.get("items"), list):
        return obj["items"]
    return []


def log(*args):
    print(*args, flush=True)


_, init = call("initialize", {"settings": {}, "locale": "zh", "theme": "light",
                              "pluginDir": ROOT, "dataDir": DATA})
log("INIT", (init or "").strip()[:300])

for kind in ("env", "hosts"):
    qid = _id + 1
    mid, q = call("query", {"queryId": qid, "command": kind, "trigger": "keyword",
                            "keyword": kind, "text": "", "raw": kind, "limit": 200})
    obj = json.loads(q) if q else {}
    items = items_of(obj)
    log("QUERY", kind, "items", len(items),
        "first", (items[0].get("id") if items else None))
    if not items:
        continue
    _, a = call("action", {"command": kind, "action": "open", "text": "", "item": items[0]})
    log("ACTION", kind, (a or "").strip()[:300])

# ---- Win32 capture ----
user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


ENUMCB = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [ENUMCB, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetWindowDC.restype = wintypes.HDC
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT,
                            wintypes.UINT, ctypes.c_void_p,
                            ctypes.POINTER(BITMAPINFOHEADER), wintypes.UINT]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]


def windows_of(pid):
    found = []

    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n > 0:
                b = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, b, n + 1)
                found.append((hwnd, b.value))
        return True

    user32.EnumWindows(ENUMCB(cb), 0)
    return found


def capture(hwnd, bmp_path):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, 2)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth, bi.biHeight = w, -h
    bi.biPlanes, bi.biBitCount, bi.biCompression = 1, 32, 0
    pixels = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, pixels, ctypes.byref(bi), 0)
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(pixels.raw), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0,
                       len(pixels.raw), 0, 0, 0, 0)
    with open(bmp_path, "wb") as f:
        f.write(hdr + info + pixels.raw)
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    return w, h


user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                               wintypes.WPARAM, wintypes.LPARAM]
WM_VSCROLL, WM_MOUSEWHEEL, WM_KEYDOWN = 0x0115, 0x020A, 0x0100
SB_PAGEDOWN = 3
VK_INSERT, VK_DELETE = 0x2D, 0x2E


def shot(hwnd, name):
    bmp = os.path.join(OUTDIR, "_%s.bmp" % name)
    png = os.path.join(OUTDIR, "editor_%s.png" % name)
    size = capture(hwnd, bmp)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", bmp, png], check=False)
    os.remove(bmp)
    log("CAPTURE", name, size, png)


time.sleep(1.2)
wins = windows_of(proc.pid)
log("WINDOWS", [(t, hex(h)) for h, t in wins])
env_hwnd = None
for hwnd, title in wins:
    key = "env" if ("环境" in title or "Environment" in title) else (
        "hosts" if "hosts" in title.lower() else None)
    if not key:
        continue
    shot(hwnd, "%s_preview" % key)
    if key == "env":
        env_hwnd = hwnd

if env_hwnd:
    # Scroll two pages: the status row must report a different visible range.
    for _ in range(2):
        user32.PostMessageW(env_hwnd, WM_VSCROLL, SB_PAGEDOWN, 0)
        time.sleep(0.15)
    time.sleep(0.4)
    shot(env_hwnd, "env_scrolled")
    # Ins adds a row and ensureVisible() must bring it into view.
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_INSERT, 0)
    time.sleep(0.5)
    shot(env_hwnd, "env_added")
    # Del removes the just-added row.
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_DELETE, 0)
    time.sleep(0.5)
    shot(env_hwnd, "env_deleted")

log("DATA_DIR", DATA)
time.sleep(900)
