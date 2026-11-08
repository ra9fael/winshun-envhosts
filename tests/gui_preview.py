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
LB_GETITEMRECT, LB_OKAY = 0x0198, 0
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
    # v0.7.2: the counter strip has a guaranteed minimum; the delimiter box
    # (the only elastic element) gives up the difference.
    commit_w, hint_min_w, gaps = 108, 230, 6 + 6 + 8
    commit_right = detail[2] - 4
    commit_left = commit_right - commit_w
    strip = max(160, commit_left - 8 - lx)
    mode_w = max(132, min(180, strip * 30 // 100))
    apply_w = max(54, min(72, strip * 14 // 100))
    rest = max(0, strip - mode_w - apply_w - gaps)
    combo_w = max(92, min(300, rest - hint_min_w))
    hint_w = max(0, rest - combo_w)
    cell_w = mode_w // 2
    hint_x = lx + mode_w + 6 + combo_w + 6 + apply_w + 8
    rects = {
        "modeRaw": (lx, mode_top, lx + cell_w, mode_top + mode_h),
        "modeList": (lx + cell_w, mode_top, lx + mode_w, mode_top + mode_h),
        "delimiter": (lx + mode_w + 6, mode_top, lx + mode_w + 6 + combo_w, mode_top + mode_h),
        "apply": (lx + mode_w + 6 + combo_w + 6, mode_top,
                  lx + mode_w + 6 + combo_w + 6 + apply_w, mode_top + mode_h),
        # v0.7.2: the item counter strip, right of the "+ 新增项" button.
        "hint": (hint_x, mode_top, hint_x + hint_w, mode_top + mode_h),
        # v0.7.0 two-stage delete: green 提交修改 button at the right end of
        # the item toolbar (detail.right - 112 .. -4).
        "commit": (commit_left, mode_top, commit_right, mode_top + mode_h),
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


# ---- read-only test hooks (needed by the env conflict section below) --------
# editor_win.h exposes two read-only test hooks. WM_APP_KEY carries an explicit
# modifier mask in lParam (bit 0 Ctrl, bit 1 Shift): injected WM_KEYDOWN cannot
# change the real GetAsyncKeyState, so Ctrl/Shift gestures need it.
WM_APP_KEY = 0x8000 + 4
WM_APP_GETSTATE = 0x8000 + 5
# v0.7.2: WM_APP_GETLAYOUT reads the painter's own geometry and colours, so the
# suite can assert that no band or strip is squeezed into an unreadable sliver.
WM_APP_GETLAYOUT = 0x8000 + 6


def state(hwnd):
    """(selected, deleted, disabled hosts rows, UI flags)."""
    v = send(hwnd, WM_APP_GETSTATE)
    return (v & 0xFFFF, (v >> 16) & 0xFFFF,
            (v >> 32) & 0xFFFF, (v >> 48) & 0xFF)


def layout_rect(hwnd, which, lp=0):
    """A rect from WM_APP_GETLAYOUT: (left, top, right, bottom)."""
    v = send(hwnd, WM_APP_GETLAYOUT, which, lp) & 0xFFFFFFFFFFFFFFFF
    return (v & 0xFFFF, (v >> 16) & 0xFFFF, (v >> 32) & 0xFFFF, (v >> 48) & 0xFFFF)


def layout_color(hwnd, which, lp=0):
    """A COLORREF (0x00BBGGRR) from WM_APP_GETLAYOUT as (r, g, b)."""
    v = send(hwnd, WM_APP_GETLAYOUT, which, lp)
    return (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)


def notice_bands(hwnd):
    """[(fg, bg), ...] for every always-on notice band the window paints."""
    out = []
    for i in range(8):
        fg = layout_color(hwnd, 10, i)
        bg = layout_color(hwnd, 11, i)
        if fg == (0, 0, 0) and bg == (0, 0, 0):
            break
        out.append((fg, bg))
    return out


def contrast_ratio(fg, bg):
    """WCAG 2.x relative-luminance contrast ratio between two RGB triples."""
    def lum(c):
        parts = []
        for v in c:
            s = v / 255.0
            parts.append(s / 12.92 if s <= 0.03928 else ((s + 0.055) / 1.055) ** 2.4)
        return 0.2126 * parts[0] + 0.7152 * parts[1] + 0.0722 * parts[2]
    a, b = lum(fg), lum(bg)
    hi, lo = max(a, b), min(a, b)
    return (hi + 0.05) / (lo + 0.05)


def post_key(hwnd, vk, ctrl=False, shift=False, alt=False):
    mods = (1 if ctrl else 0) | (2 if shift else 0) | (4 if alt else 0)
    user32.PostMessageW(hwnd, WM_APP_KEY, vk, mods)
    time.sleep(0.35)


def sel_index(hwnd):
    """The caret row (visible index) packed in bits 56-63 of GETSTATE."""
    return (send(hwnd, WM_APP_GETSTATE) >> 56) & 0xff


def diff_button_center(hwnd, which="defer"):
    """Centre of one of the three read-only diff panel buttons."""
    cw, ch = client_size(hwnd)
    pw, ph = min(760, cw - 40), min(440, ch - 80)
    px, py = (cw - pw) // 2, (ch - ph) // 2
    bw = (pw - 40) // 3
    # v0.7.2: the buttons moved up so the footer hint has a band of its own.
    by = py + ph - 64
    rects = {
        "reload": (px + 12, by, px + 12 + bw, by + 28),
        "force": (px + 16 + bw, by, px + 16 + 2 * bw, by + 28),
        "defer": (px + 20 + 2 * bw, by, px + 20 + 3 * bw, by + 28),
    }
    r = rects[which]
    return (r[0] + r[2]) // 2, (r[1] + r[3]) // 2


def dialog_text(dlg):
    """The static text of a MessageBox is control 0xFFFF."""
    static = user32.GetDlgItem(dlg, 0xFFFF)
    return get_text(static) if static else ""


def save_via_chip(hwnd, layout, answers):
    """Click the save chip and answer the dialogs the editor opens in order."""
    click_at(hwnd, *center(layout["chips"][5]))
    seen = []
    for want in answers:
        dlg = find_dialog(2.5)
        if not dlg:
            break
        press_dialog_button(dlg, want)
        # A BM_CLICK sent while the modal is still being created can be
        # swallowed; retry until the dialog actually closes.
        for _ in range(3):
            if not user32.IsWindow(dlg):
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
        shot(dlg, "dialog_save_confirm")  # the v0.7.0 themed dialog itself
        press_dialog_button(dlg, IDYES)
        # A modal MessageBox can swallow a click sent while it is still being
        # created; retry once if it did not close.
        for _ in range(3):
            if not user32.IsWindow(dlg):
                break
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

    # ---- v0.6.0: the item list owns the detail pane ------------------------
    # The 42% list + 58% action pane with a draggable vertical splitter is gone.
    prc0 = child_client_rect(env_hwnd, 10)[1]
    cw0, _ = client_size(env_hwnd)
    check(prc0[2] - prc0[0] > cw0 * 0.78,
          "item list now spans nearly the full window width",
          "%s cw=%d" % (prc0, cw0))
    check(not user32.IsWindowVisible(item_edit),
          "the separate item edit box stays hidden until in-place editing")
    shot(env_hwnd, "env_list_fullwidth")

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

    # ---- editing that item in place (v0.6.0) --------------------------------
    segs = list(PATH_SEGS)
    typed = "C:\\sample\\typed%02d" % target
    segs[target] = typed
    joined = ";".join(segs)
    user32.PostMessageW(env_hwnd, WM_COMMAND, (2 << 16) | 10, parts)  # LBN_DBLCLK
    time.sleep(0.35)
    check(user32.IsWindowVisible(item_edit),
          "double-click opens the floating in-place item editor")
    set_text(env_hwnd, item_edit, 11, typed)
    check(lb_text(parts, target) == typed, "in-place edit updates that list row", lb_text(parts, target))
    check(send(parts, LB_GETTOPINDEX) == top0, "in-place edit keeps the scroll position")
    check(send(parts, LB_GETCURSEL) == target, "in-place edit keeps the selection")
    user32.PostMessageW(item_edit, WM_KEYDOWN, VK_RETURN, 0)
    time.sleep(0.35)
    check(not user32.IsWindowVisible(item_edit), "Enter closes the in-place editor")
    shot(env_hwnd, "env_part_edited")

    # ---- v0.6.0: the space preset asks before touching Path ------------------
    # Splitting C:\Program Files\... on spaces destroys PATH, so the editor
    # confirms with No as the default before applying the space preset there.
    user32.PostMessageW(env_hwnd, WM_COMMAND, 205, 0)  # preset 空格 (Space)
    time.sleep(0.4)
    dlg_sp = find_dialog(2.5)
    check(dlg_sp is not None, "space delimiter on Path asks for confirmation first")
    if dlg_sp:
        press_dialog_button(dlg_sp, IDNO)
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == len(PATH_SEGS),
          "declining the space prompt keeps the semicolon split",
          str(send(parts, LB_GETCOUNT)))

    # ---- custom delimiter, applied explicitly -------------------------------
    # The delimiter is a flat button; its menu posts WM_COMMAND 200..206
    # (six presets incl. space, then 206 = 自定义…).
    user32.PostMessageW(env_hwnd, WM_COMMAND, 206, 0)
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
    # v0.6.0 replaced the modal Abort/Retry/Ignore dialog with a 2s watcher
    # plus a read-only three-snapshot diff panel with three whole-view actions.
    focus_row(env_hwnd, "user/LANG")
    time.sleep(0.4)
    set_text(env_hwnd, value_edit, 4, "conflict-editor-value")
    inline_env_set("LANG", "conflict-external-value")
    watcher_saw = False
    for _ in range(10):
        if state(env_hwnd)[3] & 2:
            watcher_saw = True
            break
        time.sleep(0.5)
    check(watcher_saw, "2s watcher notices an external registry write while dirty",
          str(state(env_hwnd)))
    seen = save_via_chip(env_hwnd, layout, [IDYES])
    check(seen == [IDYES], "save confirmation is still answered", str(seen))
    time.sleep(0.5)
    check(state(env_hwnd)[3] & 1 != 0,
          "save opens the read-only diff panel instead of Abort/Retry",
          str(state(env_hwnd)))
    shot(env_hwnd, "env_conflict_diff")
    # v0.7.2: the footer hint under the three actions used to be a 2px sliver
    # that the panel border cut in half. It needs a band of its own.
    panel = layout_rect(env_hwnd, 0)
    footer = layout_rect(env_hwnd, 1)
    reload_btn = layout_rect(env_hwnd, 2)
    check(footer[3] - footer[1] >= 16,
          "the diff panel footer hint has a real band, not a clipped sliver",
          "footer=%s panel=%s" % (footer, panel))
    check(footer[1] >= reload_btn[3] and footer[3] <= panel[3],
          "the diff panel footer sits below the actions and inside the panel",
          "footer=%s reload=%s panel=%s" % (footer, reload_btn, panel))
    click_at(env_hwnd, *diff_button_center(env_hwnd, "defer"))
    time.sleep(0.4)
    check(state(env_hwnd)[3] & 1 == 0, "Defer closes the diff panel",
          str(state(env_hwnd)))
    check(reg_value("LANG") == "conflict-external-value",
          "Defer leaves the external write intact", str(reg_value("LANG")))
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

    # ---- v0.6.0: the space preset splits a space-separated value ------------
    focus_row(env_hwnd, "user/EPICS_CA_ADDR_LIST")
    time.sleep(0.4)
    post_key(env_hwnd, ord("R"), ctrl=True)  # list mode
    time.sleep(0.5)
    check(send(parts, LB_GETCOUNT) == 1, "value without a preset delimiter starts whole",
          str(send(parts, LB_GETCOUNT)))
    user32.PostMessageW(env_hwnd, WM_COMMAND, 205, 0)  # preset 空格 (Space)
    time.sleep(0.4)
    dlg_ws = find_dialog(2.5)
    check(dlg_ws is not None,
          "space delimiter still confirms the whitespace normalisation")
    if dlg_ws:
        press_dialog_button(dlg_ws, IDYES)
    time.sleep(0.5)
    check(send(parts, LB_GETCOUNT) == 2, "space delimiter splits the two addresses",
          str(send(parts, LB_GETCOUNT)))
    check(lb_text(parts, 0) == "192.0.2.55" and lb_text(parts, 1) == "198.51.100.55",
          "space split yields both addresses",
          lb_text(parts, 0) + " | " + lb_text(parts, 1))
    shot(env_hwnd, "env_space_delim")
    post_key(env_hwnd, ord("R"), ctrl=True)  # back to raw for later sections
    time.sleep(0.5)

    # ---- v0.6.0 backup page: two panes with preview and actions -------------
    click_at(env_hwnd, 226, 16)  # the 备份 tab: third tab, x 184..268, y 4..28
    time.sleep(0.5)
    check(not user32.IsWindowVisible(child_by_id(env_hwnd, 4)),
          "backup tab hides the detail editing controls")
    shot(env_hwnd, "env_backup_page")
    # v0.7.1: the scope switch (全部/用户/系统) lives in the tab strip, right
    # aligned. Clicking 系统 must filter the backup list (assert via the
    # status flash text the editor renders, read back from the window title
    # is not possible - so assert the switch state via a second click round
    # trip: click 用户 then 系统 again and confirm no crash + repaint).
    def scope_center(i):
        # mirrors computeLayout: ssW=52, gap 4, right edge at cw-12
        right = cw - 12
        left2 = right - 52
        left1 = left2 - 56
        left0 = left1 - 56
        tops = 18
        return [(left0 + 26, tops), (left1 + 26, tops), (left2 + 26, tops)][i]
    for sc in (2, 1, 0):
        click_at(env_hwnd, *scope_center(sc))
        time.sleep(0.3)
    shot(env_hwnd, "env_backup_scope")
    click_at(env_hwnd, 54, 16)  # back to 用户
    time.sleep(0.4)

MK_CONTROL, MK_SHIFT = 0x0008, 0x0004
SB_TOP = 6
CF_UNICODETEXT = 13
VK_ESCAPE, VK_SPACE, VK_BACK = 0x1B, 0x20, 0x08
VK_HOME, VK_END = 0x24, 0x23
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.OpenClipboard.argtypes = [wintypes.HWND]
user32.OpenClipboard.restype = wintypes.BOOL
user32.GetClipboardData.argtypes = [wintypes.UINT]
user32.GetClipboardData.restype = ctypes.c_void_p
user32.CloseClipboard.argtypes = []
user32.IsWindowEnabled.argtypes = [wintypes.HWND]
user32.IsWindowEnabled.restype = wintypes.BOOL
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalUnlock.restype = wintypes.BOOL


def notify(method, params):
    """Send a JSON-RPC notification without waiting for a reply."""
    global _id
    _id += 1
    proc.stdin.write(json.dumps({"id": _id, "method": method, "params": params},
                                ensure_ascii=False) + "\n")
    proc.stdin.flush()


def click_row(hwnd, v, mods=0):
    """Click visible row v; table top 34, header 26, row height 24."""
    y = 34 + 26 + v * 24 + 12
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON | mods, lparam(120, y))
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam(120, y))
    time.sleep(0.3)



def clipboard_text():
    if not user32.OpenClipboard(0):
        return None
    try:
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return None
        p = kernel32.GlobalLock(h)
        if not p:
            return None
        try:
            return ctypes.wstring_at(p)
        finally:
            kernel32.GlobalUnlock(h)
    finally:
        user32.CloseClipboard()


def banner_visible(hwnd):
    """Look for the dark-red error band anywhere in the top 160px.

    PrintWindow captures the whole window, so the client-space banner is offset
    by the title bar. Scanning for the fill colour avoids hardcoding that offset.
    """
    bmp = os.path.join(OUTDIR, "_probe.bmp")
    w, h = capture(hwnd, bmp)
    with open(bmp, "rb") as f:
        data = f.read()
    os.remove(bmp)
    off = struct.unpack_from("<I", data, 10)[0]
    rows = 0
    for y in range(0, min(160, h)):
        n = 0
        for x in range(0, w, 4):
            i = off + (y * w + x) * 4
            r, g, b = data[i + 2], data[i + 1], data[i]
            # v0.6.0 hard-error band RGB(140,52,52), over-length band
            # RGB(150,58,48), external-change band RGB(150,100,20).
            if 100 <= r <= 175 and 35 <= g <= 110 and 35 <= b <= 90:
                n += 1
        if n > w // 16:
            rows += 1
    # A thin line is not a readable banner: the header used to cover the text,
    # leaving ~3px of red. Require a band tall enough to hold its message.
    return rows >= 12


def window_title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def partStagedCount(hwnd):
    """Staged split-item deletions from WM_APP_GETSTATE flags bits 3..7.

    Bits 8+ of that byte window now belong to the caret row (bits 56-63 of
    the packed state), so the staged count is masked to its five bits.
    """
    v = send(hwnd, WM_APP_GETSTATE)
    return (v >> 48) >> 3 & 0x1f


def part_action_center(hwnd, which="del"):
    """Client coordinates of the action cell in the item table.

    v0.6.0 made the item list a full-width owner-drawn table: the last column
    holds the state tag or a ×, and the list box itself handles the click. The
    caller must post to the list, not to the parent window.
    """
    parts = child_by_id(hwnd, 10)
    _, prc = child_client_rect(hwnd, 10)
    ih = send(parts, LB_GETITEMHEIGHT)
    cur = max(0, send(parts, LB_GETCURSEL))
    top = send(parts, LB_GETTOPINDEX)
    y = (cur - top) * ih + min(22, ih) // 2
    # Width must come from the list's CLIENT rect: GetWindowRect includes the
    # border and, on a long list, the vertical scrollbar covers the right edge
    # of the client area, so a window-relative x would land outside the items.
    crc = RECT()
    user32.GetClientRect(parts, ctypes.byref(crc))
    x = crc.right - 14 if which == "del" else crc.right - 60
    return x, y


def mode_is_list(hwnd):
    parts, val = child_by_id(hwnd, 10), child_by_id(hwnd, 4)
    return bool(parts and val and user32.IsWindowVisible(parts)
                and not user32.IsWindowVisible(val))


def ensure_mode(hwnd, list_mode):
    """Switch with Ctrl+R; the mode buttons move with the dragged splitter.

    Splitting a 3000-item value takes a moment, so poll until the two mutually
    exclusive controls really changed state instead of assuming 0.4 s is enough.
    """
    if mode_is_list(hwnd) != list_mode:
        post_key(hwnd, ord("R"), ctrl=True)
    deadline = time.time() + 8.0
    while time.time() < deadline:
        if mode_is_list(hwnd) == list_mode:
            return True
        time.sleep(0.15)
    return False


if env_hwnd:
    # ---- multi-select gestures, copy, batch delete and one-step undo --------
    user32.PostMessageW(env_hwnd, WM_VSCROLL, SB_TOP, 0)
    time.sleep(0.35)
    check(list_scroll_pos(env_hwnd) and list_scroll_pos(env_hwnd)[0] == 0,
          "table scrolled back to the top for selection tests",
          str(list_scroll_pos(env_hwnd)))
    click_row(env_hwnd, 1)
    click_row(env_hwnd, 2, MK_CONTROL)
    click_row(env_hwnd, 3, MK_CONTROL)
    click_row(env_hwnd, 5, MK_SHIFT)
    sel, deleted, _commented, _flags = state(env_hwnd)
    check(sel == 5 and deleted == 0,
          "Ctrl+Click and Shift+Click select a contiguous range",
          "selected=%d deleted=%d (want 5/0)" % (sel, deleted))
    shot(env_hwnd, "env_multiselect")

    post_key(env_hwnd, ord("C"), ctrl=True)
    copied = clipboard_text() or ""
    lines = copied.split("\r\n")
    check(len(lines) == 5 and all("=" in s for s in lines),
          "Ctrl+C copies exactly the selected rows", repr(copied[:160]))

    post_key(env_hwnd, VK_ESCAPE)
    check(state(env_hwnd)[0] == 0, "Esc clears the multi-selection first",
          str(state(env_hwnd)))
    post_key(env_hwnd, ord("A"), ctrl=True)
    rows_now = list_scroll_pos(env_hwnd)[1] + 1
    check(state(env_hwnd)[0] == rows_now,
          "Ctrl+A selects every visible row", "%s rows=%d" % (state(env_hwnd), rows_now))
    post_key(env_hwnd, VK_ESCAPE)

    click_row(env_hwnd, 1)
    click_row(env_hwnd, 2, MK_CONTROL)
    check(state(env_hwnd)[0] == 2, "two rows selected for the batch delete",
          str(state(env_hwnd)))
    post_key(env_hwnd, VK_DELETE)
    dlg = find_dialog(2.5)
    txt = dialog_text(dlg) if dlg else ""
    check(dlg is not None and "整个变量" in txt and "注册表" in txt,
          "deleting whole env variables asks for confirmation by name", repr(txt[:160]))
    if dlg:
        press_dialog_button(dlg, IDNO)
    check(state(env_hwnd)[1] == 0 and state(env_hwnd)[0] == 2,
          "cancelling the delete leaves the rows and selection intact",
          str(state(env_hwnd)))
    post_key(env_hwnd, VK_DELETE)
    dlg = find_dialog(2.5)
    if dlg:
        press_dialog_button(dlg, IDYES)
    time.sleep(0.4)
    check(state(env_hwnd)[1] == 2, "Del marks both selected rows deleted in one batch",
          str(state(env_hwnd)))
    shot(env_hwnd, "env_batch_deleted")

    click_at(env_hwnd, *center(layout["chips"][5]))
    dlg = find_dialog(2.5)
    txt = dialog_text(dlg) if dlg else ""
    check("将删除 2 个变量" in txt, "save lists the two variables that will be removed",
          repr(txt[:160]))
    if dlg:
        press_dialog_button(dlg, IDNO)
    post_key(env_hwnd, ord("Z"), ctrl=True)
    check(state(env_hwnd)[1] == 0,
          "one Ctrl+Z restores the whole batch (single undo transaction)",
          str(state(env_hwnd)))
    shot(env_hwnd, "env_batch_undone")

if hosts_hwnd:
    # ---- batch enable / disable on the hosts window -------------------------
    user32.PostMessageW(hosts_hwnd, WM_VSCROLL, SB_TOP, 0)
    time.sleep(0.35)
    click_row(hosts_hwnd, 2)
    click_row(hosts_hwnd, 3, MK_CONTROL)
    s0 = state(hosts_hwnd)
    check(s0[0] == 2, "two hosts rows selected for the batch toggle", str(s0))
    shot(hosts_hwnd, "hosts_multiselect")
    post_key(hosts_hwnd, VK_SPACE)
    s1 = state(hosts_hwnd)
    check(s1[2] == s0[2] + 2,
          "Space disables both selected hosts rows in one batch",
          "%s -> %s" % (s0, s1))
    click_row(hosts_hwnd, 2)
    click_row(hosts_hwnd, 3, MK_CONTROL)
    post_key(hosts_hwnd, VK_SPACE)
    s2 = state(hosts_hwnd)
    check(s2[2] == s0[2], "toggling again re-enables both rows",
          "%s -> %s" % (s0, s2))

    # ---- v0.7.1 vim-style table navigation ----------------------------------
    # j/k one row, 3j with a count prefix, G/gg first and last row, 5G row
    # five, x/dd stage-delete with the same confirmation as Del, l opens the
    # row like Enter. The plain click first collapses any multi-selection.
    click_row(hosts_hwnd, 2)
    base = sel_index(hosts_hwnd)
    check(base == 2, "plain click parks the caret on the clicked row", str(base))
    post_key(hosts_hwnd, ord("J"))
    check(sel_index(hosts_hwnd) == base + 1, "j moves the table caret down",
          str(sel_index(hosts_hwnd)))
    post_key(hosts_hwnd, ord("K"))
    check(sel_index(hosts_hwnd) == base, "k moves the table caret up",
          str(sel_index(hosts_hwnd)))
    post_key(hosts_hwnd, ord("3"))
    check(sel_index(hosts_hwnd) == base, "a bare count digit does not move the caret",
          str(sel_index(hosts_hwnd)))
    post_key(hosts_hwnd, ord("J"))
    check(sel_index(hosts_hwnd) == base + 3, "3j moves three rows down",
          str(sel_index(hosts_hwnd)))
    post_key(hosts_hwnd, ord("3"))
    post_key(hosts_hwnd, ord("K"))
    check(sel_index(hosts_hwnd) == base, "3k moves three rows back up",
          str(sel_index(hosts_hwnd)))
    post_key(hosts_hwnd, ord("G"), shift=True)
    post_key(hosts_hwnd, VK_END)
    end_sel = sel_index(hosts_hwnd)  # the Home/End oracle for "last row"
    post_key(hosts_hwnd, VK_HOME)
    home_sel = sel_index(hosts_hwnd)
    post_key(hosts_hwnd, ord("G"), shift=True)
    check(sel_index(hosts_hwnd) == end_sel, "G jumps to the last row like End",
          "%d (End=%d)" % (sel_index(hosts_hwnd), end_sel))
    post_key(hosts_hwnd, ord("g"))
    post_key(hosts_hwnd, ord("g"))  # within the 600 ms gg window
    check(sel_index(hosts_hwnd) == home_sel, "gg jumps to the first row like Home",
          "%d (Home=%d)" % (sel_index(hosts_hwnd), home_sel))
    post_key(hosts_hwnd, ord("5"))
    post_key(hosts_hwnd, ord("G"), shift=True)
    check(sel_index(hosts_hwnd) == 4, "5G jumps to row five", str(sel_index(hosts_hwnd)))
    click_row(hosts_hwnd, 3)  # a real entry: x/dd need a selectable row
    post_key(hosts_hwnd, ord("L"))
    check(state(hosts_hwnd)[2] == s2[2] + 1, "l toggles the row like Enter",
          "%s -> %s" % (s2, state(hosts_hwnd)))
    post_key(hosts_hwnd, ord("L"))
    check(state(hosts_hwnd)[2] == s2[2], "l toggles the row back off",
          str(state(hosts_hwnd)))
    # hosts rows stage-delete WITHOUT a confirmation dialog (only env whole-
    # variable deletes confirm); a second x/dd on the deleted row restores it.
    post_key(hosts_hwnd, ord("X"))
    check(state(hosts_hwnd)[1] == 1, "x marks the hosts row deleted like Del",
          str(state(hosts_hwnd)))
    post_key(hosts_hwnd, ord("X"))
    check(state(hosts_hwnd)[1] == 0, "x again restores the deleted hosts row",
          str(state(hosts_hwnd)))
    post_key(hosts_hwnd, ord("D"))
    post_key(hosts_hwnd, ord("D"))
    check(state(hosts_hwnd)[1] == 1, "dd marks the hosts row deleted like x",
          str(state(hosts_hwnd)))
    post_key(hosts_hwnd, ord("D"))
    post_key(hosts_hwnd, ord("D"))
    check(state(hosts_hwnd)[1] == 0, "dd again restores the deleted hosts row",
          str(state(hosts_hwnd)))
    shot(hosts_hwnd, "hosts_vim_nav")

if env_hwnd:
    # ---- PATH data-loss regression ------------------------------------------
    # The user report: an overlong Path, remove one split item, save, and the
    # whole variable disappeared. Exercise the exact sequence and read the
    # isolated registry key back afterwards.
    focus_row(env_hwnd, "user/Path")
    time.sleep(0.4)
    check(ensure_mode(env_hwnd, False), "editor is in raw mode for the long value")
    long_segs = ["C:\\sample\\long%04d" % i for i in range(3000)]
    long_path = ";".join(long_segs)
    # logic.h: kMaxEnvValue = 32767 UTF-16 code units. All segments are ASCII,
    # so len() is the same measurement here.
    check(len(long_path) > 32767, "test value really exceeds the 32767-unit cap",
          "utf16 units=%d" % len(long_path))
    set_text(env_hwnd, value_edit, 4, long_path)
    check(banner_visible(env_hwnd),
          "overlong value shows a visible error banner instead of failing silently")
    shot(env_hwnd, "env_path_overlong")
    # v0.7.2: the notice bands are dark and saturated, so their text colour has
    # to travel with the band. The shared "warning text" colour made the
    # message unreadable; every band must clear WCAG AA (4.5:1) now.
    bands = notice_bands(env_hwnd)
    check(len(bands) >= 1, "the over-length warning is a registered notice band",
          str(bands))
    for fg, bg in bands:
        ratio = contrast_ratio(fg, bg)
        check(ratio >= 4.5,
              "notice band text clears WCAG AA contrast on its own background",
              "fg=%s bg=%s ratio=%.2f" % (fg, bg, ratio))
    # v0.7.2: the counter strip used to be squeezed to 74px, so only the
    # character count survived and "共 N 项 · 合计 " was clipped away.
    hint = layout_rect(env_hwnd, 3)
    commit = layout_rect(env_hwnd, 4)
    check(hint[2] - hint[0] >= 200,
          "the item counter strip is wide enough for the whole counter text",
          "hint=%s" % (hint,))
    check(hint[2] <= commit[0],
          "the item counter strip never runs into the commit button",
          "hint=%s commit=%s" % (hint, commit))
    # v0.6.0: over-length is a SOFT warning. The user chose "save anyway with
    # an explicit warning", so the confirmation must appear and name the risk.
    click_at(env_hwnd, *center(layout["chips"][5]))
    dlg = find_dialog(2.5)
    txt = dialog_text(dlg) if dlg else ""
    check(dlg is not None and ("超长" in txt or "OVER-LENGTH" in txt),
          "overlong save is allowed but the confirmation names the risk",
          repr(txt[:200]))
    if dlg:
        press_dialog_button(dlg, IDNO)
    check(banner_visible(env_hwnd), "over-length warning band stays visible")

    check(ensure_mode(env_hwnd, True), "editor switches to split-item mode")
    parts = child_by_id(env_hwnd, 10)
    count_before = send(parts, LB_GETCOUNT)
    check(count_before == len(long_segs),
          "overlong value still splits into every item without truncation",
          "%d != %d" % (count_before, len(long_segs)))
    def commit_button_rect(hwnd):
        """Locate the 提交修改 button from the REAL parts-list position.

        The splitter is user-draggable earlier in this suite, so the mirrored
        detail.top can be stale. The parts list is a real child and always
        tracks the layout: list top = detail.top + 90 (valTop) + 20 (header).
        """
        _h, prc = child_client_rect(hwnd, 10)
        cw, _ch = client_size(hwnd)
        detail_top = prc[1] - 110
        detail_right = cw - 8
        mode_top = detail_top + 48
        return (detail_right - 112, mode_top, detail_right - 4, mode_top + 22)

    _px, _py = part_action_center(env_hwnd, "del")
    click_at(child_by_id(env_hwnd, 10), _px, _py)
    time.sleep(0.4)
    count_after = send(parts, LB_GETCOUNT)
    # v0.7.0 two-stage delete: the x only STAGES the item - it stays in the
    # list (red, "− 待删除") and the value is untouched until 提交修改.
    check(count_after == count_before,
          "deleting one split item only stages it (still in the list)",
          "%d -> %d" % (count_before, count_after))
    commit_rect = commit_button_rect(env_hwnd)
    click_at(env_hwnd, *center(commit_rect))
    time.sleep(0.5)
    count_committed = send(parts, LB_GETCOUNT)
    check(count_committed == count_before - 1,
          "提交修改 applies the staged item deletion",
          "%d -> %d" % (count_before, count_committed))
    check(ensure_mode(env_hwnd, False), "editor returns to raw mode after item deletion")
    # get_text() has a 16k buffer; use a large buffer for the real readback.
    big = ctypes.create_unicode_buffer(200000)
    send(value_edit, WM_GETTEXT, 199999, ctypes.byref(big))
    raw_after = big.value
    check(long_segs[0] not in raw_after and len(raw_after) < len(long_path),
          "the removed item is gone from the rejoined value, not just hidden",
          "len=%d (was %d)" % (len(raw_after), len(long_path)))
    shot(env_hwnd, "env_path_item_removed")

    # ---- v0.7.1 multi-select on the item list -------------------------------
    # Set the value to 3 items, Ctrl+Click two rows, Del stages BOTH; the
    # commit drops both; one Ctrl+Z brings all three back.
    set_text(env_hwnd, value_edit, 4, r"C:\Windows\System32;C:\Windows;C:\sample\after")
    time.sleep(0.3)
    check(ensure_mode(env_hwnd, True), "editor is in split-item mode for multiselect")
    count_ms = send(parts, LB_GETCOUNT)
    check(count_ms == 3, "three items before the multiselect check",
          str(count_ms))
    def part_row_center(hwnd, idx):
        # Clicks posted to the LIST need LIST-CLIENT coordinates (the same
        # lesson as part_action_center: GetWindowRect/parent-relative rects
        # do not line up with LB_ITEMFROMPOINT's client space).
        _h, prc = child_client_rect(hwnd, 10)
        plist = child_by_id(hwnd, 10)
        crc = RECT()
        user32.GetClientRect(plist, ctypes.byref(crc))
        ih = send(plist, LB_GETITEMHEIGHT)
        return (crc.right // 3, max(0, idx - send(plist, LB_GETTOPINDEX)) * ih + ih // 2)
    r0, r1 = part_row_center(env_hwnd, 0), part_row_center(env_hwnd, 1)
    plist = child_by_id(env_hwnd, 10)
    click_at(plist, *r0)
    time.sleep(0.2)
    user32.PostMessageW(plist, WM_LBUTTONDOWN, MK_LBUTTON | MK_CONTROL,
                        lparam(int(r1[0]), int(r1[1])))
    time.sleep(0.06)
    user32.PostMessageW(plist, WM_LBUTTONUP, 0, lparam(int(r1[0]), int(r1[1])))
    time.sleep(0.2)
    post_key(env_hwnd, VK_DELETE)  # stage both selected rows
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == 3,
          "Ctrl+Click + Del stages BOTH rows (list unchanged)", str(send(parts, LB_GETCOUNT)))
    check(partStagedCount(env_hwnd) == 2,
          "the status row reports both staged rows", partStagedCount(env_hwnd))
    commit_rect2 = commit_button_rect(env_hwnd)
    click_at(env_hwnd, *center(commit_rect2))
    time.sleep(0.5)
    check(send(parts, LB_GETCOUNT) == 1,
          "commit applies BOTH staged deletions", str(send(parts, LB_GETCOUNT)))
    shot(env_hwnd, "env_part_multiselect")
    post_key(env_hwnd, ord("Z"), ctrl=True)  # undo the whole commit
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == 3, "undo restores the three items",
          str(send(parts, LB_GETCOUNT)))

    # ---- v0.7.1 revised item keymap ------------------------------------------
    # Ctrl+N adds, Backspace stages (same as Del), Ctrl+J/K move the caret
    # WITHOUT reordering, Alt+J/K reorder, Ctrl+U deletes the whole row.
    def item_caret():
        return max(0, send(parts, LB_GETCURSEL))
    post_key(env_hwnd, ord("N"), ctrl=True)  # add an empty item after the caret
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == 4, "Ctrl+N adds one item",
          str(send(parts, LB_GETCOUNT)))
    caret_n = item_caret()
    check(caret_n == 1, "Ctrl+N puts the caret on the new item (after the old)",
          str(caret_n))
    post_key(env_hwnd, ord("K"), ctrl=True)  # caret down (no reorder)
    post_key(env_hwnd, ord("K"), ctrl=True)
    time.sleep(0.3)
    check(item_caret() == caret_n + 2, "Ctrl+K moves the caret down", str(item_caret()))
    order_before = [lb_text(parts, i) for i in range(4)]
    caret_j = item_caret()
    post_key(env_hwnd, ord("J"), alt=True)  # move the caret item up one slot
    time.sleep(0.4)
    order_after = [lb_text(parts, i) for i in range(4)]
    check(order_after[caret_j - 1] == order_before[caret_j]
          and order_after[caret_j] == order_before[caret_j - 1],
          "Alt+J moves the item up (swap with the previous slot)",
          "%s -> %s" % (order_before, order_after))
    check(item_caret() == caret_j - 1, "the caret follows the moved item",
          str(item_caret()))
    post_key(env_hwnd, ord("K"), alt=True)  # move it back down
    time.sleep(0.4)
    check([lb_text(parts, i) for i in range(4)] == order_before,
          "Alt+K moves the item back down", str([lb_text(parts, i) for i in range(4)]))
    post_key(env_hwnd, VK_BACK)  # stage-delete the caret row
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == 4 and partStagedCount(env_hwnd) == 1,
          "Backspace stages the caret item like Del",
          "count=%d staged=%d" % (send(parts, LB_GETCOUNT), partStagedCount(env_hwnd)))
    post_key(env_hwnd, VK_BACK)  # toggle back (restore)
    time.sleep(0.3)
    check(partStagedCount(env_hwnd) == 0, "Backspace again restores the staged item",
          str(partStagedCount(env_hwnd)))
    # Remove the Ctrl+N empty item: walk the caret back to it, then Backspace.
    # An ADDED item never existed in the value, so it is erased at once - no
    # commit needed.
    post_key(env_hwnd, ord("J"), ctrl=True)
    post_key(env_hwnd, ord("J"), ctrl=True)
    time.sleep(0.3)
    check(item_caret() == 1 and lb_text(parts, 1) == "",
          "Ctrl+J walks the caret back to the added empty item",
          "caret=%d item=%r" % (item_caret(), lb_text(parts, 1)))
    post_key(env_hwnd, VK_BACK)
    time.sleep(0.3)
    check(send(parts, LB_GETCOUNT) == 3,
          "Backspace on an added item erases it immediately",
          str(send(parts, LB_GETCOUNT)))
    # Bare j/k on the item list mirror the table vim keys: they move the caret
    # without Ctrl, without reordering (Alt stays the reorder modifier).
    c0 = item_caret()
    post_key(env_hwnd, ord("K"))
    check(item_caret() == min(2, c0 + 1), "bare k moves the item caret down",
          "%d -> %d" % (c0, item_caret()))
    check(send(parts, LB_GETCOUNT) == 3, "bare k does not reorder the items",
          [lb_text(parts, i) for i in range(3)])
    post_key(env_hwnd, ord("J"))
    check(item_caret() == c0, "bare j moves the item caret back up",
          str(item_caret()))

    # A whole-row delete must not leave ghost split items behind.
    short_path = r"C:\Windows\System32;C:\Windows;C:\sample\after"
    set_text(env_hwnd, value_edit, 4, short_path)
    check(not banner_visible(env_hwnd), "error banner clears once the value is valid")
    check(ensure_mode(env_hwnd, True), "editor is in split-item mode for the row-delete check")
    check(send(parts, LB_GETCOUNT) == 3, "short valid Path splits into three items",
          str(send(parts, LB_GETCOUNT)))
    # Use the explicit "delete row" chip: with the item list focused, Del is the
    # item command, not the row command (which is the whole point of the fix).
    click_at(env_hwnd, *center(layout["chips"][1]))
    dlg = find_dialog(2.5)
    if dlg:
        press_dialog_button(dlg, IDYES)
    time.sleep(0.4)
    check(send(parts, LB_GETCOUNT) == 0 and not user32.IsWindowEnabled(parts),
          "a row marked for deletion shows no stale split items",
          "count=%d enabled=%s" % (send(parts, LB_GETCOUNT),
                                   bool(user32.IsWindowEnabled(parts))))
    shot(env_hwnd, "env_path_row_deleted")
    post_key(env_hwnd, ord("Z"), ctrl=True)
    check(state(env_hwnd)[1] == 0 and send(parts, LB_GETCOUNT) == 3,
          "undo restores the row and its split items",
          str(state(env_hwnd)) + " count=%d" % send(parts, LB_GETCOUNT))

    check(ensure_mode(env_hwnd, False), "editor is in raw mode before the final save")
    seen = save_via_chip(env_hwnd, layout, [IDYES])
    check(seen == [IDYES], "short Path saves after confirmation", str(seen))
    time.sleep(0.6)
    saved_path = reg_value("Path")
    check(saved_path == short_path,
          "Path still exists after removing a split item from an overlong value",
          repr(saved_path))
    shot(env_hwnd, "env_path_saved")

    # ---- live language switching and the plugin setting override ------------
    title_zh = window_title(env_hwnd)
    enabled = child_by_id(hosts_hwnd, 5) if hosts_hwnd else None
    check("环境变量" in title_zh, "window starts in Chinese (host locale=zh)", title_zh)
    notify("appearance", {"locale": "en", "theme": "light"})
    time.sleep(0.7)
    title_en = window_title(env_hwnd)
    check("Environment variables" in title_en,
          "appearance locale=en switches an already open window to English", title_en)
    if enabled:
        check(get_text(enabled) == "Enabled",
              "native child control text switches with the language", get_text(enabled))
    shot(env_hwnd, "env_en_US")

    notify("settingsChanged", {"settings": {"uiLanguage": "zh-CN"}})
    time.sleep(0.7)
    check("环境变量" in window_title(env_hwnd),
          "uiLanguage=zh-CN overrides an English host", window_title(env_hwnd))
    notify("appearance", {"locale": "en", "theme": "light"})
    time.sleep(0.7)
    check("环境变量" in window_title(env_hwnd),
          "a pinned language survives later host appearance changes",
          window_title(env_hwnd))
    notify("settingsChanged", {"settings": {"uiLanguage": "auto"}})
    time.sleep(0.7)
    check("Environment variables" in window_title(env_hwnd),
          "auto returns to following the (English) host", window_title(env_hwnd))
    notify("appearance", {"locale": "zh", "theme": "light"})
    time.sleep(0.7)
    check("环境变量" in window_title(env_hwnd),
          "auto follows the host back to Chinese", window_title(env_hwnd))
    if enabled:
        check(get_text(enabled) == "启用", "child control text follows back to Chinese",
              get_text(enabled))
    shot(env_hwnd, "env_zh_CN")

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
