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
import sys
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
                      "192.0.2.55 198.51.100.55")
    winreg.SetValueEx(k, "LANG", 0, winreg.REG_SZ, "C")
    winreg.SetValueEx(k, "Path", 0, winreg.REG_EXPAND_SZ,
                      r"C:\Windows\System32;C:\Windows;C:\Windows\System32"
                      r"\Wbem;C:\Program Files\Git\cmd;D:\tools\Qt\bin;D:\dev\vstools"
                      r"\bin;C:\Users\sample\AppData\Local\Programs\Python\Python314")
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

LOG = os.path.join(DATA, "plugin.log")
proc = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=open(LOG, "w", encoding="utf-8", errors="replace"),
                        env=env, text=True, bufsize=1,
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
WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE = 0x0201, 0x0202, 0x0200
WM_CHAR = 0x0102
SB_PAGEDOWN = 3
VK_INSERT, VK_DELETE = 0x2D, 0x2E

user32.GetDlgCtrlID.argtypes = [wintypes.HWND]
user32.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]

ENUMCB2 = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
user32.EnumChildWindows.argtypes = [wintypes.HWND, ENUMCB2, wintypes.LPARAM]


def child_by_id(hwnd, ctrl_id):
    hit = []

    def cb(h, _):
        if user32.GetDlgCtrlID(h) == ctrl_id:
            hit.append(h)
            return False
        return True

    user32.EnumChildWindows(hwnd, ENUMCB2(cb), 0)
    return hit[0] if hit else None


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def focus_row(hwnd, ref):
    call("action", {"command": "env", "action": "open", "text": "", "item": {
        "id": "env:editor",
        "data": {"kind": "openEditor", "target": "env", "select": ref,
                 "filter": ""}}})


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

    # ---- delimited-value list mode ----
    # Row 0 of the detail panel is the multi-segment Path row. The mode button is
    # self-drawn at (right-118 .. right-4, detail.top+40 .. +58); with the default
    # 960x620 client and splitter at y=415 that is x 834-948, y 462-480.
    focus_row(env_hwnd, "user/Path")
    time.sleep(0.5)
    user32.PostMessageW(env_hwnd, WM_LBUTTONDOWN, 1, lparam(890, 470))
    user32.PostMessageW(env_hwnd, WM_LBUTTONUP, 0, lparam(890, 470))
    time.sleep(0.6)
    shot(env_hwnd, "env_list_mode")

    # Drag the splitter up ~115 px: the detail pane must grow and the list box
    # must show more items. Client coords; the splitter band sits at y 415-422.
    user32.PostMessageW(env_hwnd, WM_LBUTTONDOWN, 1, lparam(500, 418))
    for y in (400, 360, 330, 300):
        user32.PostMessageW(env_hwnd, WM_MOUSEMOVE, 1, lparam(500, y))
        time.sleep(0.05)
    user32.PostMessageW(env_hwnd, WM_LBUTTONUP, 0, lparam(500, 300))
    time.sleep(0.4)
    shot(env_hwnd, "env_splitter_dragged")

    # Insert an item after the selection, type into the item box (EN_CHANGE must
    # rejoin the value without resetting the caret), then delete it again.
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_INSERT, 0)
    time.sleep(0.3)
    part = child_by_id(env_hwnd, 11)
    log("PART_EDIT", hex(part) if part else None)
    if part:
        # Post WM_CHAR rather than SetWindowTextW: only the character path runs
        # the control's own change handling, which is what emits EN_CHANGE.
        for ch in "D:\\new\\item":
            user32.PostMessageW(part, WM_CHAR, ord(ch), 0)
            time.sleep(0.02)
        time.sleep(0.5)
    shot(env_hwnd, "env_part_edited")
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_DELETE, 0)
    time.sleep(0.4)
    shot(env_hwnd, "env_part_deleted")
    # Back to raw through the same button: the value box must show the rejoined
    # string, which proves the item edit reached the model.
    user32.PostMessageW(env_hwnd, WM_LBUTTONDOWN, 1, lparam(890, 355))
    user32.PostMessageW(env_hwnd, WM_LBUTTONUP, 0, lparam(890, 355))
    time.sleep(0.6)
    shot(env_hwnd, "env_raw_again")

log("DATA_DIR", DATA)
if "--no-hold" in sys.argv:
    proc.kill()
    proc.wait()
    with open(LOG, encoding="utf-8", errors="replace") as f:
        log("PLUGIN_LOG_TAIL", f.read()[-1200:])
else:
    time.sleep(900)
