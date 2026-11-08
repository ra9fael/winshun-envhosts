#!/usr/bin/env python3
"""Generate the SVG design drafts from editor_theme.h.

The point of this script is that the drafts cannot drift from the product.
The painter (editor_win.h) reads its colours from editor_theme.h; this
script parses the SAME file and emits docs/design/*.svg from it. A colour
change in the theme therefore moves into the draft on the next run.

Usage:
    python tools/gen_design_svg.py            # regenerate every draft
    python tools/gen_design_svg.py --check    # fail if a draft is stale

The parser understands exactly this subset of editor_theme.h:

    static constexpr COLORREF kName = ENVH_RGB(r, g, b);
    X(kName, r, g, b, dr, dg, db);

Keep new entries in that shape, or teach the parser the new form.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
THEME = os.path.join(ROOT, "editor_theme.h")
DESIGN_DIR = os.path.join(ROOT, "docs", "design")

RGB_RE = re.compile(
    r"static\s+constexpr\s+COLORREF\s+(\w+)\s*=\s*ENVH_RGB\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)"
)
INT_RE = re.compile(r"static\s+constexpr\s+int\s+(\w+)\s*=\s*(\d+)\s*;")
# The themed X-macro list: X(kName, r, g, b, dr, dg, db)
X_RE = re.compile(r"X\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,"
                  r"\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)")


def parse_theme(path):
    """Return (light, dark, metrics) parsed from editor_theme.h."""
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    # The themed roles live in the X-macro list: light rgb first, dark rgb
    # second. One list feeds both the C++ struct and these drafts.
    light, dark = {}, {}
    for name, r, g, b, dr, dg, db in X_RE.findall(text):
        light[name] = (int(r), int(g), int(b))
        dark[name] = (int(dr), int(dg), int(db))

    # One-off colours declared outside the themed list.
    for name, r, g, b in RGB_RE.findall(text):
        light.setdefault(name, (int(r), int(g), int(b)))
        dark.setdefault(name, (int(r), int(g), int(b)))

    def block(name, source):
        m = re.search(r"struct\s+" + name + r"\s*\{(.*?)\n\};", source, re.S)
        if not m:
            raise SystemExit("theme: struct %s not found" % name)
        return m.group(1)

    metrics = {}
    for name, val in INT_RE.findall(block("Metrics", text)):
        metrics[name] = int(val)

    missing = [k for k in light if k not in dark]
    if missing:
        raise SystemExit("theme: roles without a dark value: %s" % ", ".join(missing))
    return light, dark, metrics


def hexc(rgb):
    return "#%02x%02x%02x" % rgb


class Svg:
    """Minimal SVG writer with the shared theme injected as CSS classes."""

    def __init__(self, light, dark, metrics, width, height, scale=1.0):
        self.w = width
        self.h = height
        self.s = scale
        self.parts = []
        self.light = light
        self.dark = dark
        self.m = metrics

    # -- helpers ---------------------------------------------------------
    def esc(self, t):
        return (t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))

    def g(self, x, y):
        """Scale a design coordinate into SVG space."""
        return (x * self.s, y * self.s)

    def rect(self, x, y, w, h, cls="", rx=0, extra=""):
        sx, sy = self.g(x, y)
        sw, sh = w * self.s, h * self.s
        r = rx * self.s
        a = ' class="%s"' % cls if cls else ""
        b = ' rx="%g"' % r if r else ""
        self.parts.append(
            '<rect x="%g" y="%g" width="%g" height="%g"%s%s%s/>' % (sx, sy, sw, sh, a, b, extra)
        )

    def text(self, x, y, s, cls="txt", anchor="start", extra=""):
        sx, sy = self.g(x, y)
        a = ' text-anchor="%s"' % anchor if anchor != "start" else ""
        self.parts.append(
            '<text x="%g" y="%g" class="%s"%s%s>%s</text>' % (sx, sy, cls, a, extra, self.esc(s))
        )

    def line(self, x1, y1, x2, y2, cls="rowsep"):
        sx1, sy1 = self.g(x1, y1)
        sx2, sy2 = self.g(x2, y2)
        self.parts.append(
            '<line x1="%g" y1="%g" x2="%g" y2="%g" class="%s"/>' % (sx1, sy1, sx2, sy2, cls)
        )

    def circle(self, cx, cy, r, fill):
        sx, sy = self.g(cx, cy)
        self.parts.append(
            '<circle cx="%g" cy="%g" r="%g" fill="%s"/>' % (sx, sy, r * self.s, fill)
        )

    def chip(self, x, y, w, h, label, bg, fg, br):
        self.rect(x, y, w, h, rx=h / 2, extra=' fill="%s" stroke="%s" stroke-width="1"' % (bg, br))
        self.text(x + w / 2, y + h / 2 + 4, label, cls="chip-t", anchor="middle",
                  extra=' fill="%s"' % fg)

    def button(self, x, y, w, h, label, primary=False, commit=False, disabled=False):
        if disabled:
            self.rect(x, y, w, h, rx=4, extra=' fill="#f4f5f7" stroke="#e2e5ea" stroke-width="1"')
            self.text(x + w / 2, y + h / 2 + 4, label, cls="txt", anchor="middle",
                      extra=' fill="#9ca3af"')
            return
        if commit:
            bg, br, fg = hexc(self.light["kCommitBg"]), hexc(self.light["kCommitBr"]), "#ffffff"
        elif primary:
            bg, br, fg = hexc(self.light["kAccent"]), hexc(self.light["kAccentDeep"]), "#ffffff"
        else:
            bg, br, fg = "#ffffff", hexc(self.light["kBorder"]), hexc(self.light["kText"])
        self.rect(x, y, w, h, rx=4, extra=' fill="%s" stroke="%s" stroke-width="1"' % (bg, br))
        self.text(x + w / 2, y + h / 2 + 4, label, cls="txt", anchor="middle",
                  extra=' fill="%s"' % fg)

    def render(self, title):
        L = self.light
        css = """
    .bg { fill: %s; }
    .panel { fill: %s; stroke: %s; stroke-width: 1; }
    .panel-alt { fill: %s; stroke: %s; stroke-width: 1; }
    .head { fill: %s; stroke: %s; }
    .field { fill: %s; stroke: %s; }
    .tab { fill: #ffffff; stroke: %s; }
    .tab-on { fill: %s; }
    .txt { fill: %s; font-size: 13px; }
    .txt-dim { fill: %s; font-size: 12px; }
    .txt-sm { fill: %s; font-size: 11px; }
    .chip-t { font-size: 11px; }
    .hdr { fill: %s; font-size: 11px; font-weight: 600; }
    .mono { fill: %s; font-size: 12px; font-family: Consolas, monospace; }
    .rowsep { stroke: %s; }
""" % (
            hexc(L["kBg"]), hexc(L["kPanel"]), hexc(L["kBorder"]),
            hexc(L["kPanelAlt"]), hexc(L["kBorderSoft"]),
            hexc(L["kHeadBg"]), hexc(L["kBorderSoft"]),
            hexc(L["kFieldBg"]), hexc(L["kBorder"]),
            hexc(L["kBorder"]), hexc(L["kAccent"]),
            hexc(L["kText"]), hexc(L["kTextDim"]), hexc(L["kTextMuted"]),
            hexc(L["kTextDim"]), hexc(L["kText"]),
            hexc(L["kBorderSoft"]),
        )
        head = (
            '<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" '
            'viewBox="0 0 %g %g" font-family="Segoe UI, Microsoft YaHei, sans-serif">\n'
            "<title>%s</title>\n<defs><style>%s</style></defs>\n"
            % (self.w, self.h, self.w, self.h, self.esc(title), css)
        )
        return head + "\n".join(self.parts) + "\n</svg>\n"


# ---------------------------------------------------------------- drafts


def draft_backup_page(light, dark, m):
    """docs/design/backup-page-v2.svg - the backup page."""
    W, H = 960, 620
    d = Svg(light, dark, m, W, H)
    L = light

    d.rect(0, 0, W, H, cls="bg")
    d.rect(8, 8, 944, 30, cls="panel", rx=4)
    d.text(20, 28, "环境变量 — WinShun", extra=' font-weight="600"')

    # Tabs: the BACKUP tab is the active one on this page.
    d.rect(8, 46, 84, 28, cls="tab", rx=4)
    d.text(50, 65, "用户 (18)", anchor="middle", extra=' fill="#374151"')
    d.rect(96, 46, 84, 28, cls="tab", rx=4)
    d.text(138, 65, "系统 (20)", anchor="middle", extra=' fill="#374151"')
    d.rect(184, 46, 84, 28, cls="tab-on", rx=4)
    d.text(226, 65, "备份 (3)", anchor="middle", extra=' fill="#ffffff" font-weight="600"')
    d.text(280, 65, "过滤 Ctrl+F", cls="txt-dim")

    # Backup scope switch (top right).
    d.text(640, 65, "备份范围", cls="txt-sm")
    d.rect(700, 48, 52, 24, cls="tab-on", rx=12)
    d.text(726, 64, "全部", cls="chip-t", anchor="middle", extra=' fill="#ffffff" font-weight="600"')
    for x, label in ((754, "用户"), (808, "系统")):
        d.rect(x, 48, 52, 24, cls="tab", rx=12)
        d.text(x + 26, 64, label, cls="chip-t", anchor="middle", extra=' fill="#374151"')

    # Left pane.
    d.rect(8, 84, 272, 430, cls="panel-alt", rx=4)
    d.text(18, 104, "备份 (3)", extra=' font-weight="600"')
    d.text(196, 104, "自动保留最近 10 份", cls="txt-sm",
           extra=' fill="%s"' % hexc(L["kTextMuted"]))

    entries = [
        ("20261107T205602.526Z", "21:56:02 · 3 个变量", ["user", "sys"], True),
        ("20261107T204118.913Z", "20:41:18 · 1 个变量", ["user"], False),
        ("20261107T195530.221Z", "19:55:30 · 0 个变量", ["user"], False),
    ]
    y = 114
    for eid, meta, scopes, sel in entries:
        if sel:
            d.rect(10, y, 268, 44, cls="panel", rx=3,
                   extra=' fill="%s"' % hexc(L["kSelBg"]))
            d.rect(10, y, 3, 44, extra=' fill="%s"' % hexc(L["kSelBar"]))
        d.text(20, y + 18, eid, cls="mono")
        d.text(20, y + 34, meta, cls="txt-sm")
        cx = 196
        for s in scopes:
            bg, fg, br = (L["kScopeUserBg"], L["kScopeUserFg"], L["kScopeUserBr"]) if s == "user" \
                else (L["kScopeSysBg"], L["kScopeSysFg"], L["kScopeSysBr"])
            d.chip(cx, y + 6, 36, 17, "用户" if s == "user" else "系统",
                   hexc(bg), hexc(fg), hexc(br))
            cx += 40
        y += 52

    d.text(18, 500, "点击左侧备份查看差异", cls="txt-sm",
           extra=' fill="%s"' % hexc(L["kTextMuted"]))

    # Right pane.
    d.rect(288, 84, 664, 430, cls="panel", rx=4)
    d.text(300, 104, "20261107T205602.526Z", extra=' font-weight="600"')
    d.text(300, 122, "21:56:02 · 用户 2 项 + 系统 1 项 · 共 3 个变量", cls="txt-dim")
    d.rect(300, 132, 300, 24, cls="field", rx=4)
    d.text(310, 148, "搜索变量…", cls="txt-sm", extra=' fill="%s"' % hexc(L["kTextMuted"]))

    d.rect(292, 164, 656, 20, cls="head")
    d.text(300, 178, "变量", cls="hdr")
    d.text(440, 178, "范围", cls="hdr")
    d.text(510, 178, "备份中的值", cls="hdr")
    d.text(932, 178, "差异", cls="hdr", anchor="end")

    rows = [
        ("EPICS_CA_ADDR_LIST", "user", "10.10.81.255 192.168.138.255", "值不同", "kDiffValue"),
        ("Path", "user", "C:\\Users\\renfei\\.cargo\\bin;…", "值不同", "kDiffValue"),
        ("TEMP", "sys", "%USERPROFILE%\\AppData\\Local\\Temp", "值不同", "kDiffValue"),
        ("HF_TOKEN", "user", "hf_EjRWfJaUOlbdIoBV…", "相同", "kDiffSame"),
        ("LM_LICENSE_FILE", "user", "C:\\Microsemi\\License\\License.dat", "相同", "kDiffSame"),
    ]
    y = 202
    for name, scope, value, verdict, ckey in rows:
        bg, fg, br = (L["kScopeUserBg"], L["kScopeUserFg"], L["kScopeUserBr"]) if scope == "user" \
            else (L["kScopeSysBg"], L["kScopeSysFg"], L["kScopeSysBr"])
        d.text(300, y, name, cls="mono")
        d.chip(440, y - 16, 36, 17, "用户" if scope == "user" else "系统",
               hexc(bg), hexc(fg), hexc(br))
        d.text(510, y, value, cls="mono")
        d.text(932, y, verdict, anchor="end", extra=' fill="%s" font-size="12px"' % hexc(L[ckey]))
        d.line(292, y + 8, 948, y + 8)
        y += 24

    d.text(300, 500, "恢复前自动备份当前状态 · 恢复生成为未保存的改动，仍需按 Ctrl+S 写入",
           cls="txt-sm")

    # Action bar.
    d.rect(8, 524, 944, 42, cls="panel", rx=4)
    d.button(20, 532, 96, 26, "刷新")
    d.button(124, 532, 96, 26, "恢复…", primary=True)
    d.button(228, 532, 96, 26, "删除")
    d.button(332, 532, 110, 26, "导出 JSON")
    d.text(20, 590, "● 0 修改 · 选中 1 份备份 · 管理员", cls="txt-dim")
    return d.render("envhosts 备份页 v2")


def draft_editor_page(light, dark, m):
    """docs/design/staged-delete-editor.svg - the editor with staged deletes."""
    W, H = 960, 620
    d = Svg(light, dark, m, W, H)
    L = light

    d.rect(0, 0, W, H, cls="bg")
    d.rect(8, 8, 944, 30, cls="panel", rx=4)
    d.text(20, 28, "环境变量 — WinShun", extra=' font-weight="600"')

    d.rect(8, 46, 84, 28, cls="tab-on", rx=4)
    d.text(50, 65, "用户 (18)", anchor="middle", extra=' fill="#ffffff" font-weight="600"')
    d.rect(96, 46, 84, 28, cls="tab", rx=4)
    d.text(138, 65, "系统 (20)", anchor="middle", extra=' fill="#374151"')
    d.rect(184, 46, 84, 28, cls="tab", rx=4)
    d.text(226, 65, "备份 (3)", anchor="middle", extra=' fill="#374151"')
    d.text(280, 65, "过滤 Ctrl+F", cls="txt-dim")
    d.rect(600, 46, 352, 24, cls="field", rx=4)
    d.text(608, 62, "EPICS_CA_ADDR", cls="mono")

    # Main table.
    d.rect(8, 84, 944, 236, cls="panel", rx=4)
    d.rect(12, 88, 936, 20, cls="head")
    d.text(20, 102, "名称", cls="hdr")
    d.text(280, 102, "类型", cls="hdr")
    d.text(420, 102, "值", cls="hdr")
    d.text(932, 102, "状态", cls="hdr", anchor="end")

    d.text(20, 126, "chocolateyinstall", cls="mono")
    d.text(280, 126, "REG_SZ", cls="mono")
    d.text(420, 126, "C:\\Users\\renfei\\AppData\\Local\\UniGetUI\\Chocolatey", cls="mono")

    # modified row (selected)
    d.rect(12, 108, 936, 22, extra=' fill="%s"' % hexc(L["kSelBg"]))
    d.text(20, 124, "EPICS_CA_ADDR_LIST", cls="mono")
    d.text(280, 124, "REG_SZ", cls="mono")
    d.text(420, 124, "10.10.81.255 192.168.138.255 192.168.138.111:5064 192.168.138.111:5070", cls="mono")
    d.circle(884, 119, 4, hexc(L["kModified"]))
    d.text(932, 123, "● 已修改", anchor="end", extra=' fill="%s" font-size="12px"' % hexc(L["kModified"]))

    # staged-deleted row
    d.rect(12, 130, 936, 22, extra=' fill="%s"' % hexc(L["kDirtyBg"]))
    strike = ' text-decoration="line-through"'
    d.text(20, 146, "EPICS_CA_MAX_ARRAY_BYTES", cls="mono",
           extra=' fill="%s"%s' % (hexc(L["kDeleted"]), strike))
    d.text(280, 146, "REG_SZ", cls="mono", extra=' fill="%s"%s' % (hexc(L["kDeleted"]), strike))
    d.text(420, 146, "2000000", cls="mono", extra=' fill="%s"%s' % (hexc(L["kDeleted"]), strike))
    d.text(932, 145, "− 待删除", anchor="end", extra=' fill="%s" font-size="12px"' % hexc(L["kDeleted"]))

    d.text(20, 168, "HF_TOKEN", cls="mono")
    d.text(280, 168, "REG_SZ", cls="mono")
    d.text(420, 168, "hf_EjRWfJaUOlbdIoBV…", cls="mono")

    # staged-new row
    d.rect(12, 174, 936, 22, extra=' fill="%s"' % hexc(L["kNewBg"]))
    d.text(20, 190, "MY_NEW_VAR", cls="mono", extra=' fill="%s"' % hexc(L["kAdded"]))
    d.text(280, 190, "REG_SZ", cls="mono", extra=' fill="%s"' % hexc(L["kAdded"]))
    d.text(420, 190, "hello", cls="mono", extra=' fill="%s"' % hexc(L["kAdded"]))
    d.text(932, 189, "+ 待新增", anchor="end", extra=' fill="%s" font-size="12px"' % hexc(L["kAdded"]))

    d.text(20, 212, "Path", cls="mono")
    d.text(280, 212, "REG_EXPAND_SZ", cls="mono")
    d.text(420, 212, "C:\\Users\\renfei\\.cargo\\bin;…", cls="mono")

    d.text(20, 234, "共 18 项 · 1 项已修改 · 1 项待删除 · 1 项待新增", cls="txt-sm")
    d.text(760, 234, "删除仅标记，提交后才写入注册表", cls="txt-sm",
           extra=' fill="%s"' % hexc(L["kTextMuted"]))

    # Detail pane.
    d.rect(8, 330, 944, 200, cls="panel", rx=4)
    d.text(20, 352, "名称", cls="txt-sm")
    d.rect(20, 358, 330, 24, cls="field", rx=4)
    d.text(28, 374, "EPICS_CA_ADDR_LIST", cls="mono")
    d.text(370, 352, "类型", cls="txt-sm")
    d.rect(370, 358, 330, 24, cls="field", rx=4)
    d.text(378, 374, "REG_SZ", cls="mono")
    d.button(740, 356, 110, 26, "提交修改", commit=True)
    d.button(858, 356, 84, 26, "撤销全部")

    d.rect(20, 398, 80, 24, cls="tab-on", rx=4)
    d.text(60, 414, "原文", anchor="middle", extra=' fill="#ffffff" font-weight="600"')
    d.rect(104, 398, 80, 24, cls="tab", rx=4)
    d.text(144, 414, "分隔项", anchor="middle", extra=' fill="#374151"')
    d.rect(200, 398, 420, 24, cls="field", rx=4)
    d.text(210, 414, "分号 (;)", cls="txt-sm")
    d.button(628, 398, 80, 24, "+ 新增项")
    d.text(724, 414, "共 8 项 · 合计 74 / 32,767 字符", cls="txt-sm")

    # Item table.
    d.rect(20, 430, 920, 20, cls="head")
    d.text(28, 444, "#", cls="hdr")
    d.text(80, 444, "分隔项", cls="hdr")
    d.text(860, 444, "字符", cls="hdr", anchor="end")
    d.text(932, 444, "操作", cls="hdr", anchor="end")

    d.text(28, 466, "01", cls="mono")
    d.text(80, 466, "10.10.81.255", cls="mono")
    d.text(860, 466, "12", cls="mono", anchor="end")
    d.text(932, 466, "×", cls="txt-sm", anchor="end", extra=' fill="%s"' % hexc(L["kTextMuted"]))

    d.text(28, 488, "02", cls="mono")
    d.text(80, 488, "192.168.138.255", cls="mono")
    d.text(860, 488, "15", cls="mono", anchor="end")
    d.text(932, 488, "×", cls="txt-sm", anchor="end", extra=' fill="%s"' % hexc(L["kTextMuted"]))

    d.rect(20, 494, 920, 22, extra=' fill="%s"' % hexc(L["kDirtyBg"]))
    d.text(28, 510, "03", cls="mono", extra=' fill="%s"%s' % (hexc(L["kDeleted"]), strike))
    d.text(80, 510, "192.168.138.111:5064", cls="mono",
           extra=' fill="%s"%s' % (hexc(L["kDeleted"]), strike))
    d.text(860, 510, "20", cls="mono", anchor="end",
           extra=' fill="%s"%s' % (hexc(L["kDeleted"]), strike))
    d.text(932, 508, "− 待删除", anchor="end", extra=' fill="%s" font-size="11px"' % hexc(L["kDeleted"]))

    d.rect(20, 516, 920, 22, extra=' fill="%s"' % hexc(L["kNewBg"]))
    d.text(28, 532, "04", cls="mono", extra=' fill="%s"' % hexc(L["kAdded"]))
    d.text(80, 532, "192.168.138.111:5070", cls="mono", extra=' fill="%s"' % hexc(L["kAdded"]))
    d.text(860, 532, "20", cls="mono", anchor="end", extra=' fill="%s"' % hexc(L["kAdded"]))
    d.text(932, 530, "+ 待新增", anchor="end", extra=' fill="%s" font-size="11px"' % hexc(L["kAdded"]))

    # Bottom bar.
    d.rect(8, 540, 944, 40, cls="panel", rx=4)
    d.text(20, 558, "● 3 项待提交 · 1 项已修改 · 1 项待删除 · 1 项待新增", cls="txt-dim")
    d.text(256, 558, "管理员：用户 / 系统均可写", cls="txt-sm")
    d.button(20, 564, 110, 22, "提交修改 Ctrl+S", commit=True)
    d.button(138, 564, 80, 22, "撤销")
    d.button(226, 564, 70, 22, "重做")
    return d.render("envhosts 编辑界面 · 暂存删除")


def draft_dialog(light, dark, m):
    """docs/design/custom-dialog.svg - the themed modal dialog (v2).

    v1 complaints: square buttons, a harsh gray block behind the body text,
    a rigid rectangular window. v2: rounded window (10px), no header band -
    a clean white sheet with a tinted icon tile, body text straight on the
    background, the detail block with an accent bar on its left edge, and
    rounded buttons with a hairline divider above the button row.
    """
    W, H = 960, 620
    d = Svg(light, dark, m, W, H)
    L = light

    d.rect(0, 0, W, H, extra=' fill="#e9ebef"')

    def dialog(ox, oy, title, subtitle, body, code, icon, buttons):
        DW = 560
        d.rect(ox, oy, DW, 260, cls="panel", rx=10)
        # icon tile: rounded square, soft tint, glyph centred
        fill = {"danger": "#fee2e2", "warn": "#fef3c7", "info": "#dbeafe"}[icon]
        br = {"danger": "#fca5a5", "warn": "#fcd34d", "info": "#93c5fd"}[icon]
        d.rect(ox + 24, oy + 24, 40, 40, rx=10,
               extra=' fill="%s" stroke="%s" stroke-width="1"' % (fill, br))
        d.text(ox + 44, oy + 50, {"danger": "!", "warn": "⚠", "info": "i"}[icon],
               anchor="middle", extra=' font-size="16" font-weight="700" fill="%s"'
               % {"danger": "#dc2626", "warn": "#b45309", "info": "#2563eb"}[icon])
        d.text(ox + 80, oy + 40, title, extra=' font-size="15" font-weight="600"')
        d.text(ox + 80, oy + 58, subtitle, cls="txt-dim")
        # body straight on the background (v1 had a gray block here)
        d.text(ox + 24, oy + 96, body)
        # detail block: accent bar on the left edge, soft panel, mono text
        d.rect(ox + 24, oy + 122, DW - 48, 40, cls="panel-alt", rx=4)
        d.rect(ox + 24, oy + 122, 3, 40, rx=1,
               extra=' fill="%s"' % hexc(L["kAccent"]))
        d.text(ox + 36, oy + 138, code, cls="mono")
        # hairline above the button row
        d.line(ox + 24, oy + 196, ox + DW - 24, oy + 196, cls="rowsep")
        bx = ox + DW - 20
        for label, kind in reversed(buttons):
            w = 108 if kind in ("danger", "primary") else 84
            bx -= w
            if kind == "danger":
                d.rect(bx, oy + 212, w, 30, rx=6,
                       extra=' fill="#dc2626" stroke="#b91c1c" stroke-width="1"')
                d.text(bx + w / 2, oy + 232, label, anchor="middle",
                       extra=' fill="#ffffff" font-size="13px" font-weight="600"')
            elif kind == "primary":
                d.rect(bx, oy + 212, w, 30, rx=6,
                       extra=' fill="%s" stroke="%s" stroke-width="1"'
                       % (hexc(L["kAccent"]), hexc(L["kAccentDeep"])))
                d.text(bx + w / 2, oy + 232, label, anchor="middle",
                       extra=' fill="#ffffff" font-size="13px" font-weight="600"')
            else:
                d.rect(bx, oy + 212, w, 30, rx=6, cls="panel")
                d.text(bx + w / 2, oy + 232, label, anchor="middle")
            bx -= 10

    dialog(
        200, 40, "标记删除整个变量", "提交修改后才会写入注册表",
        "即将标记删除 EPICS_CA_ADDR_LIST，包含 8 个分隔项。",
        "提交修改（Ctrl+S）后从注册表移除该变量",
        "danger", [("取消", "plain"), ("标记删除", "danger")],
    )
    dialog(
        200, 330, "确认保存", "写入注册表前的最后一步",
        "保存 1 项修改：Path 的值将更新。",
        "~ Path = C:\\Users\\renfei\\.cargo\\bin;…",
        "info", [("取消", "plain"), ("确认保存", "primary")],
    )
    return d.render("envhosts 自定义弹窗 v2")


DRAFTS = [
    ("backup-page-v2.svg", draft_backup_page),
    ("staged-delete-editor.svg", draft_editor_page),
    ("custom-dialog.svg", draft_dialog),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero when a draft is out of date")
    args = ap.parse_args()

    light, dark, metrics = parse_theme(THEME)
    os.makedirs(DESIGN_DIR, exist_ok=True)

    stale = []
    for name, fn in DRAFTS:
        path = os.path.join(DESIGN_DIR, name)
        content = fn(light, dark, metrics)
        if args.check:
            old = ""
            if os.path.exists(path):
                with open(path, "r", encoding="utf-8") as fh:
                    old = fh.read()
            if old != content:
                stale.append(name)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(content)
            print("wrote %s" % name)

    if args.check and stale:
        print("STALE: %s" % ", ".join(stale), file=sys.stderr)
        return 1
    if args.check:
        print("design drafts are up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
