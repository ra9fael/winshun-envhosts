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
import zlib
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# A real-install run (tests/verify_install.py) points these at the installed
# plugin folder and a scratch output folder; the defaults keep the normal
# development preview unchanged.
EXE = os.environ.get("WS_ENVHOSTS_EXE") or os.path.join(ROOT, "envhosts.exe")
PLUGIN_DIR = os.environ.get("WS_ENVHOSTS_PLUGIN_DIR") or ROOT
OUTDIR = os.environ.get("WS_ENVHOSTS_OUTDIR") or os.path.join(HERE, "fixtures")
os.makedirs(OUTDIR, exist_ok=True)
DATA = tempfile.mkdtemp(prefix="ws-envhosts-preview-")
SUBKEY = r"Software\WinShunTest\EnvHosts"
# The system scope is redirected too: without this the 系统 tab would screenshot
# this machine's real HKLM variables into the repository.
SYS_SUBKEY = r"Software\WinShunTest\EnvHostsSystem"
# EnvStore takes the root from an explicit "HKCU\" prefix, so the 系统 tab reads
# these fake rows instead of this machine's real HKLM variables.
SYS_OVERRIDE = "HKCU\\" + SYS_SUBKEY
SYS_ROWS = 6
with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, SYS_SUBKEY, 0, winreg.KEY_SET_VALUE) as k:
    for i in range(1, 6):
        winreg.SetValueEx(k, "WS_SYS_%02d" % i, 0, winreg.REG_SZ, "sample system row %02d" % i)
    winreg.SetValueEx(k, "Path", 0, winreg.REG_EXPAND_SZ,
                      r"%SystemRoot%\system32;%SystemRoot%;C:\sample\sys")

# A hosts preview with many real entries plus two group headings, so scrolling
# and both row kinds can be verified. Kept in the temp data dir, never in the
# real System32 hosts file.
HOSTS = os.path.join(DATA, "hosts.preview")
with open(HOSTS, "w", encoding="utf-8", newline="\r\n") as f:
    f.write("# sample hosts for GUI preview\n")
    f.write("#\n")
    f.write("#      102.54.94.97     rhino.acme.com          # source server\n")
    for i in range(1, 25):
        f.write("10.%d.%d.%d host%02d.example.com host%02d  # row %02d\n"
                % (i // 10, i, i, i, i, i))
    f.write("# trailing group\n")
    f.write("127.0.0.1 localhost\n")
    f.write("127.0.0.2 host03.example.com\n")  # duplicate of line 6, flagged '!'

# Preview-only sample rows in the isolated test subkey. The real user
# Environment key and the real hosts file are never touched.
# 28 delimiter-separated segments, all fake, so the item list really scrolls.
PATH_SEGS = [r"C:\Windows\System32", r"C:\Windows", r"C:\Windows\System32\Wbem"] \
    + ["C:\\sample\\dir%02d" % i for i in range(1, 26)]
PATH_SAMPLE = ";".join(PATH_SEGS)
with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, SUBKEY, 0, winreg.KEY_SET_VALUE) as k:
    winreg.SetValueEx(k, "EPICS_CA_ADDR_LIST", 0, winreg.REG_SZ,
                      "192.0.2.55 198.51.100.55")
    winreg.SetValueEx(k, "LANG", 0, winreg.REG_SZ, "C")
    winreg.SetValueEx(k, "Path", 0, winreg.REG_EXPAND_SZ, PATH_SAMPLE)
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
env["WS_ENVHOSTS_TEST_SYSTEM_SUBKEY"] = SYS_OVERRIDE
env["WS_ENVHOSTS_TEST_HOSTS"] = HOSTS

LOG = os.path.join(DATA, "plugin.log")
proc = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=open(LOG, "w", encoding="utf-8", errors="replace"),
                        env=env, text=True, bufsize=1, cwd=os.path.dirname(os.path.abspath(EXE)),
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
                              "pluginDir": PLUGIN_DIR, "dataDir": DATA})
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
WM_GETTEXT, WM_SETTEXT = 0x000D, 0x000C
WM_VSCROLL, WM_MOUSEWHEEL, WM_KEYDOWN = 0x0115, 0x020A, 0x0100
WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE = 0x0201, 0x0202, 0x0200
WM_CHAR = 0x0102
WM_COMMAND = 0x0111
EN_CHANGE = 768
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


def bmp_to_png(bmp_path, png_path):
    """Encode the 32-bit top-down BGRA DIB from capture() without ffmpeg.

    The GDI DIB leaves the alpha byte undefined, so it is forced to 0xff;
    otherwise every PNG would come out fully transparent.
    """
    with open(bmp_path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = abs(struct.unpack_from("<i", data, 22)[0])
    if struct.unpack_from("<H", data, 28)[0] != 32:
        raise ValueError("capture() must produce a 32-bit DIB")
    stride = w * 4
    pixels = data[off:off + stride * h]
    scanlines = bytearray()
    opaque = b"\xff" * w
    for y in range(h):
        row = bytearray(pixels[y * stride:(y + 1) * stride])
        blue = row[0::4]
        row[0::4] = row[2::4]  # BGR(A) -> RGB(A)
        row[2::4] = blue
        row[3::4] = opaque
        scanlines.append(0)    # PNG filter type: None
        scanlines.extend(row)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    with open(png_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(bytes(scanlines), 6))
                + chunk(b"IEND", b""))


def shot(hwnd, name):
    bmp = os.path.join(OUTDIR, "_%s.bmp" % name)
    png = os.path.join(OUTDIR, "editor_%s.png" % name)
    size = capture(hwnd, bmp)
    bmp_to_png(bmp, png)
    os.remove(bmp)
    log("CAPTURE", name, size, png)


# ---- messaging, layout mirror and assertions ---------------------------------
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, ctypes.c_void_p]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetDlgItem.restype = wintypes.HWND
user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int

WM_GETTEXTLENGTH = 0x000E
LB_GETCOUNT, LB_GETCURSEL, LB_GETTEXT = 0x018B, 0x0188, 0x0189
LB_GETITEMHEIGHT, LB_SETTOPINDEX, LB_GETTOPINDEX = 0x01A1, 0x0197, 0x018E
CB_GETCOMBOBOXINFO = 0x0164
BM_CLICK = 0x00F5
IDABORT, IDRETRY, IDIGNORE, IDYES, IDNO, IDOK = 3, 4, 5, 6, 7, 1
SB_VERT, SIF_RANGE, SIF_PAGE, SIF_POS = 1, 0x01, 0x02, 0x04
MK_LBUTTON = 0x0001
VK_CONTROL, VK_SHIFT, VK_RETURN, VK_F5, VK_INSERT, VK_DELETE = 0x11, 0x10, 0x0D, 0x74, 0x2D, 0x2E


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class COMBOBOXINFO(ctypes.Structure):
    """The real winuser.h layout: three rects/flags then the three child handles.

    cbSize must match sizeof() exactly or CB_GETCOMBOBOXINFO fails silently, so
    this cannot carry extra state fields.
    """
    _fields_ = [("cbSize", wintypes.DWORD), ("rcItem", RECT), ("rcButton", RECT),
                ("stateButton", wintypes.DWORD), ("hwndCombo", wintypes.HWND),
                ("hwndItem", wintypes.HWND), ("hwndList", wintypes.HWND)]


class SCROLLINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("fMask", wintypes.UINT),
                ("nMin", ctypes.c_int), ("nMax", ctypes.c_int),
                ("nPage", wintypes.UINT), ("nPos", ctypes.c_int),
                ("nTrackPos", ctypes.c_int)]


user32.GetScrollInfo.restype = wintypes.BOOL
user32.GetScrollInfo.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.POINTER(SCROLLINFO)]
user32.ScreenToClient.argtypes = [wintypes.HWND, ctypes.POINTER(POINT)]


FAILURES = []


def check(cond, label, extra=""):
    log("PASS" if cond else "FAIL", label, "" if cond else extra)
    if not cond:
        FAILURES.append(label)


def send(hwnd, msg, wp=0, lp=0):
    return int(user32.SendMessageW(hwnd, msg, wp, lp))


def get_text(hwnd):
    """Read a field with WM_GETTEXT; cross-process GetWindowTextW truncates."""
    buf = ctypes.create_unicode_buffer(16384)
    send(hwnd, WM_GETTEXT, 16383, ctypes.byref(buf))
    return buf.value


def set_text(parent, edit, ctrl_id, text, notify=True):
    """Write a field, then notify the way the UI would.

    The editor only commits detail edits from the EN_CHANGE handler, so a set
    without it leaves the row clean and turns save into a no-op.
    """
    buf = ctypes.create_unicode_buffer(text)
    send(edit, WM_SETTEXT, 0, ctypes.byref(buf))
    if notify:
        user32.PostMessageW(parent, WM_COMMAND, (EN_CHANGE << 16) | ctrl_id, edit)
    time.sleep(0.35)


def client_size(hwnd):
    r = RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right, r.bottom


def child_client_rect(hwnd, ctrl_id):
    h = child_by_id(hwnd, ctrl_id)
    if not h:
        return None, None
    wr = RECT()
    user32.GetWindowRect(h, ctypes.byref(wr))
    tl, br = POINT(wr.left, wr.top), POINT(wr.right, wr.bottom)
    user32.ScreenToClient(hwnd, ctypes.byref(tl))
    user32.ScreenToClient(hwnd, ctypes.byref(br))
    return h, (tl.x, tl.y, br.x, br.y)


def list_scroll_pos(hwnd):
    si = SCROLLINFO()
    si.cbSize = ctypes.sizeof(SCROLLINFO)
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS
    if not user32.GetScrollInfo(hwnd, SB_VERT, ctypes.byref(si)):
        return None
    return si.nPos, si.nMax, si.nPage


def mirror_layout(cw, ch):
    """Recompute the editor's rects from the same formulas as computeLayout().

    Only the self-drawn bands (mode segment, apply, splitter, status chips) need
    this; the real child controls are measured from their own windows and the
    result is cross-checked against the delimiter combo.
    """
    work_top, status_h, splitter_h = 34, 48, 7
    min_list_h, min_detail_h = 112, 200
    work_bottom = ch - status_h
    splitter_y = max(work_top + min_list_h, work_bottom - min_detail_h - splitter_h)
    detail = (8, splitter_y + splitter_h, cw - 8, work_bottom)
    lx, mode_h = detail[0] + 4, 22
    mode_top = detail[1] + 48
    available = max(160, detail[2] - lx - 208)
    mode_w = max(132, min(180, available * 30 // 100))
    apply_w = max(54, min(72, available * 14 // 100))
    combo_w = max(92, available - mode_w - apply_w - 12)
    cell_w = mode_w // 2
    rects = {
        "modeRaw": (lx, mode_top, lx + cell_w, mode_top + mode_h),
        "modeList": (lx + cell_w, mode_top, lx + mode_w, mode_top + mode_h),
        "delimiter": (lx + mode_w + 6, mode_top, lx + mode_w + 6 + combo_w, mode_top + mode_h),
        "apply": (lx + mode_w + 6 + combo_w + 6, mode_top,
                  lx + mode_w + 6 + combo_w + 6 + apply_w, mode_top + mode_h),
        "splitter": (8, splitter_y, cw - 8, splitter_y + splitter_h),
        "detail": detail,
    }
    widths = [72, 72, 82, 68, 68, 108, 72]  # add, del, edit, undo, redo, save, close
    x, y = 8, work_bottom + 24 + 4
    chips = []
    for w in widths:
        if x + w > cw - 8:
            break
        chips.append((x, y, x + w, y + max(14, ch - (work_bottom + 24) - 8 - 4)))
        x += w + 4
    rects["chips"] = chips
    return rects


def center(rect):
    return ((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2)


def click_at(hwnd, x, y):
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam(int(x), int(y)))
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam(int(x), int(y)))
    time.sleep(0.3)


def lb_text(list_hwnd, index):
    buf = ctypes.create_unicode_buffer(4096)
    if send(list_hwnd, LB_GETTEXT, index, ctypes.byref(buf)) < 0:
        return None
    return buf.value


def combo_edit_hwnd(combo):
    ci = COMBOBOXINFO()
    ci.cbSize = ctypes.sizeof(COMBOBOXINFO)
    if send(combo, CB_GETCOMBOBOXINFO, 0, ctypes.byref(ci)) == 0:
        return None
    return ctypes.cast(ci.hwndItem, wintypes.HWND).value or 0


def pid_windows():
    """Every top-level window of the plugin: handle, class, caption.

    Filtering on caption or visibility here is what made the earlier run miss
    the reload prompt, so dialogs are matched on the class alone.
    """
    out = []

    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == proc.pid:
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, cls, 64)
            n = user32.GetWindowTextLengthW(hwnd)
            cap = ctypes.create_unicode_buffer(n + 2)
            if n > 0:
                user32.GetWindowTextW(hwnd, cap, n + 1)
            out.append((hwnd, cls.value, cap.value, bool(user32.IsWindowVisible(hwnd))))
        return True

    user32.EnumWindows(ENUMCB(cb), 0)
    return out


def find_dialog(timeout=4.0):
    end = time.time() + timeout
    seen = []
    while time.time() < end:
        seen = pid_windows()
        for hwnd, cls, _cap, vis in seen:
            if cls == "#32770" and vis:
                return hwnd
        time.sleep(0.1)
    log("DIALOG_TIMEOUT", [(hex(h), c, t, v) for h, c, t, v in seen])
    return None


def press_dialog_button(hwnd, ctrl_id):
    btn = user32.GetDlgItem(hwnd, ctrl_id)
    if not btn:
        return False
    send(btn, BM_CLICK)
    time.sleep(0.4)
    return True


def inline_env_set(name, value):
    """Write through the launcher protocol, i.e. an external change for the window."""
    item = {"id": "env:edit:" + name.lower(),
            "data": {"kind": "envEdit", "name": name, "value": value}}
    return call("action", {"command": "env", "action": "save", "text": "", "item": item})


def reg_value(name):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SUBKEY) as k:
        return winreg.QueryValueEx(k, name)[0]


def save_via_chip(hwnd, layout, answers):
    """Click the save chip and answer the dialogs the editor opens in order."""
    click_at(hwnd, *center(layout["chips"][5]))
    seen = []
    for want in answers:
        dlg = find_dialog(2.5)
        if not dlg:
            break
        press_dialog_button(dlg, want)
        seen.append(want)
    return seen


time.sleep(1.2)
wins = windows_of(proc.pid)
log("WINDOWS", [(t, hex(h)) for h, t in wins])
env_hwnd = None
hosts_hwnd = None
for hwnd, title in wins:
    key = "env" if ("环境" in title or "Environment" in title) else (
        "hosts" if "hosts" in title.lower() else None)
    if not key:
        continue
    shot(hwnd, "%s_preview" % key)
    if key == "env":
        env_hwnd = hwnd
    else:
        hosts_hwnd = hwnd

if hosts_hwnd:
    cw, ch = client_size(hosts_hwnd)
    detail_top = ch - 48 - 146
    rects = {}
    for ctrl_id, label in ((5, "enabled"), (6, "IP"), (7, "hostnames"), (8, "comment")):
        hwnd, rect = child_client_rect(hosts_hwnd, ctrl_id)
        rects[label] = rect
        check(hwnd and user32.IsWindowVisible(hwnd), "hosts %s field is visible" % label)
        check(rect and rect[1] >= detail_top and rect[3] <= ch - 48,
              "hosts %s field stays inside detail panel" % label, str(rect))
    check(rects["enabled"][2] <= rects["IP"][0]
          and rects["IP"][2] <= rects["hostnames"][0]
          and rects["hostnames"][2] <= rects["comment"][0],
          "hosts fields are ordered without overlap", str(rects))
    enabled = child_by_id(hosts_hwnd, 5)
    before = send(enabled, 0x00F0)
    send(enabled, BM_CLICK)
    after = send(enabled, 0x00F0)
    send(enabled, BM_CLICK)
    restored = send(enabled, 0x00F0)
    check(after != before and restored == before,
          "hosts enabled checkbox toggles and restores", "%s -> %s -> %s" % (before, after, restored))
    shot(hosts_hwnd, "hosts_detail_styled")

    # ---- raw tab: one scrollbar, fills the work area, editable ---------------
    click_at(hosts_hwnd, 138, 16)  # 原文 tab
    time.sleep(0.4)
    raw_h = child_by_id(hosts_hwnd, 9)
    raw_txt = get_text(raw_h)
    check("host01.example.com" in raw_txt and raw_txt.count("\n") >= 25,
          "hosts raw tab shows the whole file", repr(raw_txt[:80]))
    GWL_STYLE, WS_VSCROLL_BIT, ES_READONLY_BIT = -16, 0x00200000, 0x0800
    user32.GetWindowLongW.restype = ctypes.c_long
    check(not (user32.GetWindowLongW(hosts_hwnd, GWL_STYLE) & WS_VSCROLL_BIT),
          "raw tab hides the window's own scrollbar (only the text box scrolls)")
    _, rrc = child_client_rect(hosts_hwnd, 9)
    hcw, hch = client_size(hosts_hwnd)
    status_top = hch - 48
    check(rrc and 0 <= status_top - rrc[3] <= 8, "raw box reaches the status bar",
          "%s status_top=%d" % (rrc, status_top))
    check(not (user32.GetWindowLongW(raw_h, GWL_STYLE) & ES_READONLY_BIT),
          "raw box is editable when hosts is writable")
    shot(hosts_hwnd, "hosts_raw")
    # Edit one line in the raw text, then leave the tab: the entry must show it.
    edited = raw_txt.replace("host05.example.com host05", "rawedit.example.com host05")
    set_text(hosts_hwnd, raw_h, 9, edited)  # WM_SETTEXT: SetWindowTextW can't cross processes
    time.sleep(0.2)
    shot(hosts_hwnd, "hosts_raw_edited")
    click_at(hosts_hwnd, 54, 16)  # back to 条目 -> applies the raw text
    time.sleep(0.5)
    check(not user32.IsWindowVisible(raw_h), "raw box hides on the entries tab")
    check(user32.GetWindowLongW(hosts_hwnd, GWL_STYLE) & WS_VSCROLL_BIT,
          "entries tab brings the window scrollbar back")
    # Minimise the window, then re-open through the launcher: the editor must
    # restore itself (showAndActivate) instead of staying minimised. Whether it
    # also takes the foreground depends on Windows' focus rights for the
    # plugin process, so only the restore is asserted here.
    user32.ShowWindow(hosts_hwnd, 6)  # SW_MINIMIZE
    time.sleep(0.3)
    check(user32.IsIconic(hosts_hwnd) != 0, "hosts window is minimised for the re-open test")
    focus_hosts_line = 8  # host05 is on line 8 (3 header lines + host01..04)
    call("action", {"command": "hosts", "action": "open", "text": "", "item": {
        "id": "hosts:editor", "data": {"kind": "openEditor", "target": "hosts",
                                       "select": str(focus_hosts_line), "filter": ""}}})
    time.sleep(0.5)
    check(user32.IsIconic(hosts_hwnd) == 0, "re-opening restores a minimised editor")
    time.sleep(0.5)
    check(get_text(child_by_id(hosts_hwnd, 7)) == "rawedit.example.com host05",
          "raw edit shows up as the entry on that line", get_text(child_by_id(hosts_hwnd, 7)))
    shot(hosts_hwnd, "hosts_after_raw_edit")
    # Save through the Ctrl+S chip and read the temp file back.
    hlayout = mirror_layout(hcw, hch)
    click_at(hosts_hwnd, *center(hlayout["chips"][5]))
    dlg = find_dialog(2.5)
    check(dlg is not None, "hosts save asks for confirmation")
    if dlg:
        press_dialog_button(dlg, IDYES)
    time.sleep(0.6)
    with open(HOSTS, "rb") as f:
        saved = f.read()
    check(b"rawedit.example.com host05" in saved and saved.count(b"\n") == saved.count(b"\r\n"),
          "save writes the raw edit to the hosts file with CRLF kept", repr(saved[:120]))
    shot(hosts_hwnd, "hosts_saved_after_raw_edit")

if env_hwnd:
    cw, ch = client_size(env_hwnd)
    layout = mirror_layout(cw, ch)
    combo_h, combo_rc = child_client_rect(env_hwnd, 12)
    check(combo_h and not user32.IsWindowVisible(combo_h),
          "custom delimiter box stays hidden until 自定义… is chosen")

    # ---- table scrolling ----------------------------------------------------
    pos0 = list_scroll_pos(env_hwnd)
    for _ in range(2):
        user32.PostMessageW(env_hwnd, WM_VSCROLL, SB_PAGEDOWN, 0)
        time.sleep(0.15)
    time.sleep(0.4)
    pos1 = list_scroll_pos(env_hwnd)
    check(pos0 and pos1 and pos1[0] > pos0[0], "page-down scrolls the table", f"{pos0} -> {pos1}")
    shot(env_hwnd, "env_scrolled")

    # ---- row CRUD -----------------------------------------------------------
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_INSERT, 0)
    time.sleep(0.5)
    n1 = list_scroll_pos(env_hwnd)
    check(n1 and n1[1] == pos1[1] + 1, "Ins adds one row", f"{pos1} -> {n1}")
    shot(env_hwnd, "env_added")
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_DELETE, 0)
    time.sleep(0.4)
    n2 = list_scroll_pos(env_hwnd)
    check(n2 and n2[1] == pos1[1], "Del removes it again", f"{n1} -> {n2}")
    shot(env_hwnd, "env_deleted")

    # ---- delimited item editing on the sample Path --------------------------
    focus_row(env_hwnd, "user/Path")
    time.sleep(0.5)
    click_at(env_hwnd, *center(layout["modeList"]))
    time.sleep(0.4)
    parts = child_by_id(env_hwnd, 10)
    item_edit = child_by_id(env_hwnd, 11)
    value_edit = child_by_id(env_hwnd, 4)
    check(bool(parts) and user32.IsWindowVisible(parts), "item list shows in list mode")
    check(not user32.IsWindowVisible(value_edit), "raw value box hides in list mode")
    count = send(parts, LB_GETCOUNT)
    check(count == len(PATH_SEGS), "every segment becomes an item", f"{count} != {len(PATH_SEGS)}")
    shot(env_hwnd, "env_list_mode")

    # ---- vertical splitter between the item list and the action pane --------
    # The grip is a 7px band starting 4px right of the LISTBOX's right edge.
    prc0 = child_client_rect(env_hwnd, 10)[1]
    erc0 = child_client_rect(env_hwnd, 11)[1]
    sx, sy = prc0[2] + 7, (prc0[1] + prc0[3]) // 2
    user32.PostMessageW(env_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam(sx, sy))
    for dx in (40, 80, 120):
        user32.PostMessageW(env_hwnd, WM_MOUSEMOVE, MK_LBUTTON, lparam(sx + dx, sy))
        time.sleep(0.05)
    user32.PostMessageW(env_hwnd, WM_LBUTTONUP, 0, lparam(sx + 120, sy))
    time.sleep(0.4)
    prc1 = child_client_rect(env_hwnd, 10)[1]
    erc1 = child_client_rect(env_hwnd, 11)[1]
    check(prc1[2] - prc1[0] > prc0[2] - prc0[0] + 60,
          "dragging the vertical splitter widens the item list",
          f"{prc0} -> {prc1}")
    check(erc1[0] >= prc1[2] + 8 and erc1[2] - erc1[0] < erc0[2] - erc0[0],
          "action pane follows the split and stays clear of the list",
          f"{erc0} -> {erc1}")
    shot(env_hwnd, "env_parts_splitter_dragged")
    # Restore the default proportions so the later assertions keep their
    # mirrored layout coordinates.
    sx1 = prc1[2] + 7
    user32.PostMessageW(env_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam(sx1, sy))
    for dx in (-40, -80, -120):
        user32.PostMessageW(env_hwnd, WM_MOUSEMOVE, MK_LBUTTON, lparam(sx1 + dx, sy))
        time.sleep(0.05)
    user32.PostMessageW(env_hwnd, WM_LBUTTONUP, 0, lparam(sx1 - 120, sy))
    time.sleep(0.4)

    # ---- selection must stay where it is scrolled to ------------------------
    top0 = 8
    send(parts, LB_SETTOPINDEX, top0, 0)
    ih = send(parts, LB_GETITEMHEIGHT)
    prc = child_client_rect(env_hwnd, 10)[1]
    target = top0 + max(1, (prc[3] - prc[1]) // ih - 2)
    click_at(parts, 20, (target - top0) * ih + ih // 2)
    time.sleep(0.4)
    cur = send(parts, LB_GETCURSEL)
    top_after = send(parts, LB_GETTOPINDEX)
    check(cur == target, "click selects the item under the pointer", f"cur={cur} want={target}")
    check(top_after == top0, "selection keeps its scroll position", f"top {top0} -> {top_after}")
    shot(env_hwnd, "env_item_selected")

    # ---- editing that item --------------------------------------------------
    segs = list(PATH_SEGS)
    typed = "C:\\sample\\typed%02d" % target
    segs[target] = typed
    joined = ";".join(segs)
    set_text(env_hwnd, item_edit, 11, typed)
    check(lb_text(parts, target) == typed, "item box write updates that list row", lb_text(parts, target))
    check(send(parts, LB_GETTOPINDEX) == top0, "item edit keeps the scroll position")
    check(send(parts, LB_GETCURSEL) == target, "item edit keeps the selection")
    shot(env_hwnd, "env_part_edited")

    # ---- custom delimiter, applied explicitly -------------------------------
    # The delimiter is a flat button; its menu posts WM_COMMAND 200..205.
    # 205 = 自定义… shows the edit box in the button's place.
    user32.PostMessageW(env_hwnd, WM_COMMAND, 205, 0)
    time.sleep(0.4)
    ced = combo_h
    _, ced_rc = child_client_rect(env_hwnd, 12)
    check(user32.IsWindowVisible(ced) and ced_rc and ced_rc[1] == layout["delimiter"][1],
          "自定义… shows the delimiter box in the button slot", str(ced_rc))
    set_text(env_hwnd, ced, 0, "=>", notify=False)
    user32.PostMessageW(ced, WM_KEYDOWN, VK_RETURN, 0)
    time.sleep(0.4)
    check(not user32.IsWindowVisible(ced), "Enter applies and hides the custom box")
    check(send(parts, LB_GETCOUNT) == 1, "custom delimiter re-splits the whole value",
          str(send(parts, LB_GETCOUNT)))
    check(lb_text(parts, 0) == joined, "delimiter switch keeps the value intact", lb_text(parts, 0))
    shot(env_hwnd, "env_delimiter_custom")
    user32.PostMessageW(env_hwnd, WM_COMMAND, 200, 0)  # preset 分号 (;) applies at once
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == len(segs), "delimiter switch back restores the items",
          str(send(parts, LB_GETCOUNT)))
    check(lb_text(parts, target) == typed, "item text survives the delimiter round trip",
          lb_text(parts, target))

    # ---- back to raw: the value box must show the rejoined value ------------
    click_at(env_hwnd, *center(layout["modeRaw"]))
    time.sleep(0.4)
    check(user32.IsWindowVisible(value_edit), "raw value box visible again")
    raw_now = get_text(value_edit)
    check(raw_now == joined, "raw box shows the rejoined value after item edits", raw_now[:160])
    shot(env_hwnd, "env_raw_again")

    # ---- splitter drag ------------------------------------------------------
    v0 = child_client_rect(env_hwnd, 4)[1]
    sy = center(layout["splitter"])[1]
    user32.PostMessageW(env_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam(cw // 2, sy))
    for y in (sy - 25, sy - 50, sy - 75):
        user32.PostMessageW(env_hwnd, WM_MOUSEMOVE, MK_LBUTTON, lparam(cw // 2, y))
        time.sleep(0.05)
    user32.PostMessageW(env_hwnd, WM_LBUTTONUP, 0, lparam(cw // 2, sy - 75))
    time.sleep(0.4)
    v1 = child_client_rect(env_hwnd, 4)[1]
    check(v1[3] - v1[1] > v0[3] - v0[1], "dragging the splitter grows the value field",
          f"{v0} -> {v1}")
    shot(env_hwnd, "env_splitter_dragged")

    # ---- real save, then read the isolated registry back --------------------
    seen = save_via_chip(env_hwnd, layout, [IDYES])
    check(seen == [IDYES], "save confirmation dialog answered", str(seen))
    time.sleep(0.6)
    check(reg_value("Path") == joined, "save wrote the edited Path to the registry",
          str(reg_value("Path"))[:160])
    shot(env_hwnd, "env_saved")

    # ---- system scope survives the save -------------------------------------
    click_at(env_hwnd, 138, 16)  # the 系统 tab: fixed at x 96..180, y 4..28
    time.sleep(0.4)
    syspos = list_scroll_pos(env_hwnd)
    check(syspos is not None and syspos[1] + 1 >= SYS_ROWS, "system rows survive a save",
          "%s rows=%s" % (syspos, None if syspos is None else syspos[1] + 1))
    shot(env_hwnd, "env_system_after_save")
    click_at(env_hwnd, 54, 16)  # back to 用户
    time.sleep(0.4)

    # ---- conflict: external write while the window has unsaved edits --------
    focus_row(env_hwnd, "user/LANG")
    time.sleep(0.4)
    set_text(env_hwnd, value_edit, 4, "conflict-editor-value")
    inline_env_set("LANG", "conflict-external-value")
    time.sleep(0.5)
    seen = save_via_chip(env_hwnd, layout, [IDYES, IDABORT])
    check(seen == [IDYES, IDABORT], "conflict is surfaced before overwriting", str(seen))
    check(reg_value("LANG") == "conflict-external-value",
          "Abort leaves the external write intact", str(reg_value("LANG")))
    shot(env_hwnd, "env_conflict_aborted")

    # ---- F5 reload asks first, then reads the external value back -----------
    user32.PostMessageW(env_hwnd, WM_KEYDOWN, VK_F5, 0)
    dlg = find_dialog(2.5)
    check(dlg is not None, "F5 with unsaved edits asks before discarding")
    if dlg:
        press_dialog_button(dlg, IDYES)
    time.sleep(0.5)
    check(get_text(value_edit) == "conflict-external-value",
          "reload reads the external value back", get_text(value_edit))
    shot(env_hwnd, "env_after_reload")

log("DATA_DIR", DATA)
log("CHECKS", "failures=%d" % len(FAILURES), ",".join(FAILURES) if FAILURES else "all passed")
if "--no-hold" in sys.argv:
    proc.kill()
    proc.wait()
    with open(LOG, encoding="utf-8", errors="replace") as f:
        log("PLUGIN_LOG_TAIL", f.read()[-1200:])
    # The preview rows live in isolated subkeys; drop them so a normal run does
    # not leave sample values behind.
    for sub in (SUBKEY, SYS_SUBKEY):
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, sub)
        except FileNotFoundError:
            pass
    sys.exit(1 if FAILURES else 0)
else:
    time.sleep(900)
