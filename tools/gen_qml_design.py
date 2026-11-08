#!/usr/bin/env python3
"""Generate reviewable design boards and QML tokens from the shared source.

Run: python tools/gen_qml_design.py [--check]
Only Python's standard library is required. All example values are synthetic.
"""
from pathlib import Path
from html import escape
import argparse
import json

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "qml" / "design"
TOKENS = json.loads((OUT.parent / "design-tokens.json").read_text(encoding="utf-8"))
LIGHT = TOKENS["colors"]["light"]
DARK = TOKENS["colors"]["dark"]


class Board:
    def __init__(self, page, title, subtitle, dark=False):
        self.c = DARK if dark else LIGHT
        self.page = page
        self.title = title
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="1040" viewBox="0 0 1440 1040" role="img" aria-labelledby="title desc">',
                      f'<title id="title">{escape(page + " · " + title)}</title>',
                      f'<desc id="desc">{escape(subtitle)}。QML 目标设计示意，并非运行截图，数据均为虚构。</desc>',
                      '<defs><filter id="shadow" x="-10%" y="-10%" width="120%" height="135%"><feDropShadow dx="0" dy="8" stdDeviation="12" flood-color="#12233a" flood-opacity=".08"/></filter></defs>']
        self.rect(0, 0, 1440, 1040, "bg", radius=0)
        self.text(40, 39, "ENVHOSTS  /  QML DESIGN", 13, "accent", weight=700)
        self.text(1400, 39, page + "   ·   2026.11.08", 13, "muted", anchor="end")
        self.text(40, 82, title, 30, weight=700)
        self.text(40, 112, subtitle, 15, "muted")

    def color(self, color):
        return self.c.get(color, color)

    def rect(self, x, y, w, h, fill="panel", stroke=None, radius=10, extra=""):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{self.color(fill)}"' +
                          (f' stroke="{self.color(stroke)}"' if stroke else '') + f' {extra}/>')

    def line(self, x1, y1, x2, y2, color="line", width=1):
        self.parts.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{self.color(color)}" stroke-width="{width}"/>')

    def text(self, x, y, text, size=15, color="ink", weight=400, anchor="start", mono=False):
        family = 'Cascadia Code,Consolas,Microsoft YaHei UI,monospace' if mono else 'Segoe UI,Microsoft YaHei UI,Microsoft YaHei,sans-serif'
        self.parts.append(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}" fill="{self.color(color)}">{escape(str(text))}</text>')

    def button(self, x, y, w, label, primary=False, danger=False, disabled=False, h=36, selected=False):
        fill = "field" if disabled else "accent" if primary else "soft" if selected else "redbg" if danger else "field"
        ink = "muted" if disabled else "onAccent" if primary else "accent" if selected else "red" if danger else "ink"
        self.rect(x, y, w, h, fill, "control" if not (primary or selected) else None, 7)
        self.text(x + w / 2, y + h / 2 + 5, label, 14, ink, 600, "middle")

    def badge(self, x, y, label, color="accent", w=None):
        w = w or 28 + len(label) * 13
        bg = {"accent": "soft", "green": "greenbg", "amber": "amberbg", "red": "redbg"}.get(color, "field")
        self.rect(x, y, w, 26, bg, radius=6)
        self.text(x + 10, y + 18, label, 13, color, 600)

    def field(self, x, y, w, label, value, mono=False):
        self.text(x, y, label, 13, "muted")
        self.rect(x, y + 10, w, 40, "field", "control", 6)
        self.text(x + 12, y + 36, value, 14, mono=mono)

    def shell(self, kind="env", active="变量", dirty=2):
        self.rect(40, 136, 1360, 780, "panel", "line", 14, 'filter="url(#shadow)"')
        self.badge(60, 148, "eh", w=38)
        self.text(110, 167, "EnvHosts  /  " + ("环境变量" if kind == "env" else "hosts 文件"), 14, weight=600)
        self.text(1368, 166, "—     □     ×", 18, "muted", anchor="end")
        self.line(40, 184, 1400, 184)
        self.text(64, 221, "环境变量" if kind == "env" else "hosts 文件", 23, weight=700)
        self.badge(202, 198, f"{dirty} 项未保存" if dirty else "已与目标同步", "amber" if dirty else "green", 130)
        self.button(1018, 197, 94, "重新载入")
        self.button(1122, 197, 90, "撤销  ↶")
        self.button(1222, 197, 154, "保存到系统  Ctrl+S", True, disabled=not dirty)
        labels = ["变量", "备份"] if kind == "env" else ["条目", "原文", "备份"]
        for i, label in enumerate(labels):
            x = 64 + i * 100
            self.text(x + 12, 271, label, 15, "accent" if active == label else "muted", 600)
            if active == label:
                self.line(x, 287, x + 74, 287, "accent", 3)
        self.line(40, 290, 1400, 290)
        self.line(40, 852, 1400, 852)
        self.text(64, 877, "● 未保存的修改仅保存在工作副本" if dirty else "✓ 已同步", 13, "amber" if dirty else "green")
        self.text(1376, 877, "用户范围可写  ·  系统范围只读" if kind == "env" else "当前文件可写  ·  保留原始格式", 13, "muted", anchor="end")
        self.text(64, 903, "Ctrl+F  筛选     F2  编辑     Ctrl+Z / Y  撤销 / 重做     F5  重载     Tab  切换焦点", 12, "muted")

    def notes(self, items):
        for i, (title, detail) in enumerate(items):
            x = 40 + 460 * i
            self.text(x, 955, title, 15, "accent", 700)
            self.text(x, 982, detail, 13, "muted")
        self.text(40, 1020, "设计示意 · 合成数据 · 实际实现和验证范围见 IMPLEMENTATION.md", 12, "muted")
        self.text(1400, 1020, self.page, 12, "muted", anchor="end")

    def done(self):
        return "\n".join(self.parts + ['</svg>', ''])


def env_board(dark=False):
    b = Board("P08" if dark else "P01", "环境变量 · 从列表到原值编辑", "清晰区分作用域、数据类型与未保存状态；列表与详情上下分栏。", dark)
    b.shell()
    b.rect(64, 306, 640, 36, "field", "line", 7)
    b.text(78, 330, "⌕  筛选名称或值  ·  Ctrl+F", 14, "muted")
    b.button(720, 306, 220, "全部范围  /  用户  /  系统")
    b.button(1122, 306, 110, "+ 新增变量")
    b.button(1242, 306, 134, "删除整个变量", danger=True)
    b.rect(64, 355, 1312, 32, "field", radius=5)
    for x, label in [(80, "状态"), (160, "名称"), (420, "作用域"), (560, "类型"), (800, "值")]:
        b.text(x, 377, label, 12, "muted", 600)
    rows = [("●", "Path", "用户", "REG_EXPAND_SZ", r"%USERPROFILE%\bin;C:\Tools\bin", "amber"),
            ("+", "DEV_MODE", "用户", "REG_SZ", "local", "green"),
            ("", "JAVA_HOME", "用户", "REG_SZ", r"C:\SDK\jdk-21", "muted"),
            ("锁", "Path", "系统", "REG_EXPAND_SZ", r"%SystemRoot%\System32;…", "muted")]
    for i, (state, name, scope, typ, value, color) in enumerate(rows):
        y = 390 + i * 40
        if i == 0:
            b.rect(64, y, 1312, 40, "soft", radius=5)
            b.rect(64, y + 5, 3, 30, "accent", radius=1)
        b.text(82, y + 26, state, 14, color, 700)
        b.text(160, y + 26, name, 14, weight=600)
        b.text(420, y + 26, scope, 14)
        b.text(560, y + 26, typ, 13, "muted", mono=True)
        b.text(800, y + 26, value, 14, mono=True)
        b.line(64, y + 40, 1376, y + 40)
    b.line(64, 569, 1376, 569)
    b.rect(690, 566, 60, 5, "line", radius=2)
    b.text(64, 605, "Path", 21, weight=700)
    b.badge(126, 584, "用户 · 已修改", "amber", 118)
    b.text(1376, 605, "当前选中 1 项  ·  列表 1–4 / 4", 13, "muted", anchor="end")
    b.field(64, 633, 390, "变量名称", "Path")
    b.field(470, 633, 250, "数据类型", "REG_EXPAND_SZ  ▾")
    b.button(1056, 641, 154, "复制原值")
    b.button(1220, 641, 156, "复制展开值")
    b.button(64, 699, 94, "原值", selected=True)
    b.button(168, 699, 124, "分隔项编辑")
    value_length = len(r"%USERPROFILE%\bin;C:\Tools\bin".encode("utf-16-le")) // 2
    b.text(1376, 722, f"合计 {value_length} / 32,767 UTF-16 单元", 13, "muted", anchor="end")
    b.rect(64, 746, 1312, 62, "field", "line", 7)
    b.text(78, 776, r"%USERPROFILE%\bin;C:\Tools\bin", 16, mono=True)
    b.text(64, 833, "输入会更新工作副本；只有“保存到系统”确认后才写入。", 13, "muted")
    b.notes([("F03 / F04  选择与编辑", "名称 + 作用域识别变量；滚动和筛选不改变编辑目标。"),
             ("F07  长值反馈", "警告常驻；空名、非法字符等硬错误阻止保存。"),
             ("F17 / F18  主题与焦点", "明暗主题使用同一布局；焦点环与选中状态分开。")])
    return b


def split_board():
    b = Board("P02", "分隔项编辑 · 自动暂存，统一保存", "编辑自动进入工作副本，切换视图无需提交；保存确认后才写入系统。")
    b.shell(dirty=1)
    b.text(64, 325, "当前变量", 13, "muted")
    b.text(64, 360, "Path", 26, weight=700)
    b.badge(140, 337, "用户 / REG_EXPAND_SZ", w=208)
    b.button(1114, 327, 262, "删除整个变量", danger=True)
    b.rect(64, 390, 1312, 55, "amberbg", radius=8)
    b.text(80, 413, "修改已暂存，尚未写入系统", 15, "amber", 700)
    b.text(80, 434, "待删除项保留在列表，可恢复；切换变量、筛选和页签不会丢失编辑。", 13, "amber")
    b.button(64, 465, 86, "原值")
    b.button(160, 465, 116, "分隔项", selected=True)
    b.button(296, 465, 180, "分隔符：分号 ;  ▾")
    b.button(496, 465, 112, "+ 新增项")
    b.text(1376, 489, "可撤销  ·  Ctrl+Z", 14, "muted", anchor="end")
    b.rect(64, 521, 1312, 34, "field", radius=5)
    for x, label in [(82, "状态"), (166, "分隔项值"), (1114, "操作")]:
        b.text(x, 544, label, 13, "muted")
    rows = [("", r"%USERPROFILE%\bin", "muted", "上移   下移   删除"),
            ("●", r"C:\Tools\bin", "amber", "上移   下移   删除"),
            ("−", r"C:\OldSDK\bin", "red", "恢复此项"),
            ("+", r"C:\SDK\bin", "green", "上移   下移   移除")]
    for i, (state, value, color, actions) in enumerate(rows):
        y = 559 + i * 49
        b.rect(64, y, 1312, 45, "redbg" if i == 2 else "soft" if i == 1 else "field", radius=6)
        b.text(82, y + 29, state, 17, color, 700)
        b.text(166, y + 29, value, 16, "red" if i == 2 else "ink", mono=True)
        if i == 2:
            b.line(165, y + 23, 295, y + 23, "red")
        b.text(1114, y + 28, actions, 14, "accent")
    value_length = len(r"%USERPROFILE%\bin;C:\Tools\bin;C:\SDK\bin".encode("utf-16-le")) // 2
    b.text(64, 785, f"共 4 项  ·  有效 3 项  ·  待删除 1 项  ·  合计 {value_length} / 32,767", 14, "muted")
    b.text(64, 818, "Del  标记所选分隔项     Alt+↑ / ↓  移动     Enter  编辑 / 确认     Esc  取消本次输入", 13, "muted")
    b.notes([("F05  删除语义", "已有项标记删除；新增项直接移除；均可撤销。"),
             ("F06  分隔符切换", "仅归一化等有损操作需确认；普通切换无弹窗。"),
             ("F11  唯一保存入口", "Ctrl+S 展示完整变更；取消后工作副本仍保留。")])
    return b


def hosts_board(raw=False):
    b = Board("P04" if raw else "P03", "hosts 原文 · 原始格式可核对" if raw else "hosts 条目 · 映射与注释一目了然", "原文与条目共享工作副本；未改动行、BOM、换行和末尾换行均需保真。")
    b.shell("hosts", "原文" if raw else "条目", 1)
    if raw:
        b.badge(64, 306, "UTF-8 + BOM", w=138)
        b.badge(212, 306, "CRLF", w=80)
        b.badge(302, 306, "末尾换行：有", w=140)
        b.text(1376, 326, r"C:\Fixtures\hosts", 13, "muted", anchor="end", mono=True)
        b.rect(64, 352, 1312, 426, "field", "line", 8)
        b.rect(64, 352, 58, 426, "bg", radius=8)
        lines = ["# EnvHosts sample — synthetic fixture", "# Local development", "127.0.0.1 localhost", "::1 localhost-v6", "", "# Development services", "127.0.0.1 api.test auth.test # local gateway", "# 10.0.0.24 preview.test", "10.0.0.25 api.test # duplicate hostname", ""]
        for i, line in enumerate(lines):
            y = 385 + i * 34
            b.text(102, y, i + 1, 14, "muted", anchor="end", mono=True)
            b.text(140, y, line, 16, "muted" if line.startswith("#") else "ink", mono=True)
        b.text(64, 811, "原文编辑中：离开此页或保存时合并成一次模型撤销。", 14, "amber")
        b.text(64, 837, "Ctrl+Z / Y 在文本区仅作用于原文输入；切回条目后可撤销整次原文修改。", 13, "muted")
    else:
        b.rect(64, 306, 800, 36, "field", "line", 7)
        b.text(78, 330, "⌕  筛选 IP、主机名或注释", 14, "muted")
        b.button(1078, 306, 140, "+ 新增映射")
        b.button(1228, 306, 148, "删除所选行", danger=True)
        b.rect(64, 356, 1312, 32, "field", radius=5)
        for x, label in [(80, "行号 / 状态"), (238, "启用"), (342, "IP 地址"), (604, "主机名"), (1050, "注释")]:
            b.text(x, 378, label, 13, "muted")
        b.text(82, 414, "▸ 文件头注释（2 行）", 14, "muted")
        rows = [("3", "开", "127.0.0.1", "localhost", ""), ("4", "开", "::1", "localhost-v6", ""),
                ("6", "", "", "# Development services", ""),
                ("7  ●", "开", "127.0.0.1", "api.test  auth.test", "local gateway"),
                ("8", "关", "10.0.0.24", "preview.test", ""), ("9  !", "开", "10.0.0.25", "api.test", "重复启用映射")]
        for i, row in enumerate(rows):
            y = 426 + i * 30
            if i == 3:
                b.rect(64, y, 1312, 30, "soft", radius=4)
            for j, (x, value) in enumerate(zip([82, 238, 342, 604, 1050], row)):
                b.text(x, y + 21, value, 14, "red" if i == 5 else "muted" if i in (2, 4) else "ink", mono=j in (2, 3))
        b.line(64, 615, 1376, 615)
        b.text(64, 649, "第 7 行 · 2 个主机名", 20, weight=700)
        b.badge(310, 629, "已修改", "amber", 86)
        b.field(64, 683, 300, "IP 地址", "127.0.0.1", True)
        b.field(380, 683, 610, "主机名（空格分隔）", "api.test auth.test", True)
        b.button(1010, 693, 170, "启用：开", selected=True)
        b.button(1196, 693, 180, "复制整行")
        b.field(64, 757, 1312, "行尾注释", "local gateway")
        b.text(64, 836, "! 发现 1 个重复主机名映射；该提示不自动删除或修改任何行。", 13, "red")
    b.notes([("F08 / F09  同一份数据", "条目页与原文页往返；注释和未知行留在文件中。"),
             ("F10  实际写权限", "只读仍可选择、搜索、复制；修改控件禁用并说明原因。"),
             ("T18–T23  字节校验", "比较真实字节，不只比较解析后显示的文字。")])
    return b


def backup_board():
    b = Board("P05", "备份 · 先看差异，再载入工作副本", "恢复是可撤销的编辑操作；真正写回仍走统一保存确认。")
    b.shell(active="备份", dirty=0)
    b.text(64, 328, "环境变量备份  ·  3 份", 17, weight=700)
    b.text(400, 328, "2026-11-08 14:32:08  ·  用户范围", 17, weight=600)
    b.line(376, 306, 376, 831)
    b.text(1376, 328, "与当前工作副本比较", 13, "muted", anchor="end")
    for i, (time, scope) in enumerate([("14:32:08", "用户 · 3 个变量"), ("11:18:42", "用户 + 系统 · 12 个变量"), ("09:05:11", "用户 · 4 个变量")]):
        y = 351 + i * 98
        b.rect(64, y, 288, 82, "soft" if i == 0 else "field", "line", 8)
        b.text(80, y + 29, "2026-11-08  " + time, 15, weight=600)
        b.text(80, y + 58, scope, 13, "muted")
    b.button(64, 641, 288, "范围：全部  ▾")
    b.text(64, 700, "时间按本地时区显示", 13, "muted")
    b.text(64, 724, "备份 ID 保持原样", 13, "muted")
    b.button(64, 784, 130, "刷新列表")
    b.rect(400, 351, 976, 38, "field", "line", 7)
    b.text(414, 376, "⌕  搜索备份中的名称或值", 14, "muted")
    b.text(400, 426, "= 相同 1    ~ 值不同 1    + 仅备份有 1    − 当前多出 1", 14, "muted")
    rows = [("~", "Path", "值不同", "amber"), ("=", "JAVA_HOME", "相同", "muted"), ("+", "OLD_SDK", "仅备份有 · 恢复时新增", "green"), ("−", "DEV_MODE", "当前多出 · 恢复时删除", "red")]
    for i, (sym, name, label, color) in enumerate(rows):
        y = 446 + i * 43
        b.rect(400, y, 976, 39, "soft" if i == 0 else "field", radius=5)
        b.text(414, y + 26, sym, 16, color, 700)
        b.text(452, y + 26, name, 15, weight=600)
        b.text(1050, y + 26, label, 13, color)
    b.field(400, 652, 976, "备份中的 Path", r"%USERPROFILE%\bin;C:\SDK\bin", True)
    b.text(400, 735, "此备份只包含用户范围；系统变量不会进入本次恢复计划。", 14, "muted")
    b.button(400, 784, 150, "导出 JSON")
    b.button(560, 784, 132, "删除备份…", danger=True)
    b.button(1146, 784, 230, "恢复到工作副本…", True)
    b.notes([("F14  恢复范围", "逐范围展示增改删；恢复前保护当前工作副本。"),
             ("F14 / F11  二次核对", "恢复后显示未保存；保存时再备份当前系统值。"),
             ("T35–T38  失败可辨", "空备份、损坏文件、无法读取必须是不同结果。")])
    return b


def review_board():
    b = Board("P06", "保存与冲突 · 明确展示将发生什么", "确认内容绑定工作副本版本与目标指纹；任何变化都必须重新审阅。")
    b.rect(40, 155, 660, 741, "panel", "line", 12)
    b.rect(724, 155, 676, 741, "panel", "line", 12)
    b.badge(64, 177, "D01 · 保存确认", w=162)
    b.text(64, 243, "保存 3 项环境变量变更", 24, weight=700)
    b.text(64, 275, "目标：用户范围  ·  写入前创建备份", 15, "muted")
    for y, sym, name, info, color in [(312, "+", "DEV_MODE", "新增：local", "green"), (402, "~", "Path", "更新：2 个分隔项 → 3 个分隔项", "amber"), (492, "−", "OLD_SDK", "将删除整个变量", "red")]:
        b.rect(64, y, 612, 76, "field", "line", 7)
        b.text(80, y + 29, sym, 20, color, 700)
        b.text(116, y + 29, name, 16, weight=600)
        b.text(116, y + 57, info, 14, color)
    b.rect(64, 593, 612, 76, "amberbg", radius=8)
    b.text(80, 621, "删除 1 个变量：OLD_SDK", 15, "red", 700)
    b.text(80, 647, "需要展开原值时可逐条查看；取消会保留编辑。", 13, "amber")
    b.text(64, 708, "保存期间显示进度并禁用重复提交。", 14, "muted")
    b.text(64, 736, "备份失败或校验失败：不开始目标写入。", 14, "muted")
    b.button(286, 817, 150, "返回编辑")
    b.button(446, 817, 230, "确认保存 3 项", True)
    b.badge(748, 177, "D02 · 外部冲突", "amber", 174)
    b.text(748, 243, "目标已被外部修改", 24, weight=700)
    b.text(748, 275, "基线 / 我的修改 / 当前系统  ·  只读对比", 15, "muted")
    b.rect(748, 312, 628, 48, "amberbg", radius=7)
    b.text(764, 343, "Path  ·  冲突：双方改动且结果不同", 16, "amber", 600)
    for y, label, value in [(393, "基线", r"C:\Tools\bin"), (488, "我的修改", r"C:\Tools\bin;C:\SDK\bin"), (583, "当前系统", r"C:\Tools\bin;C:\Other\bin")]:
        b.field(748, y, 628, label, value, True)
    b.text(748, 696, "重新载入会丢弃我的修改，需再次确认。", 14, "muted")
    b.text(748, 723, "覆盖按本次变更集执行，保留外部无关项。", 14, "muted")
    b.text(748, 750, "覆盖前仍重新校验、备份；不得绕过权限。", 14, "muted")
    b.button(748, 817, 152, "暂不处理")
    b.button(910, 817, 190, "重新载入…")
    b.button(1110, 817, 266, "用我的修改覆盖…", danger=True)
    b.notes([("F11 / F12  再次校验", "确认后再读目标，避免审阅期间的旧指纹失效。"),
             ("F12  不静默合并", "选择覆盖后重建确认；处理期间再次变化则重开差异。"),
             ("T27–T32  写入故障", "逐项结果可核对；部分失败不显示全部成功。")])
    return b


def states_board():
    b = Board("P07", "异常与恢复 · 有明确的下一步", "空、只读、校验失败、外部变化与恢复草稿各有独立状态，不复用模糊的错误空屏。")
    cards = [
        ("S01", "没有匹配的变量", "muted", ["当前筛选：sdk", "数据仍在工作副本中，清除筛选即可查看。"], "清除筛选"),
        ("S02", "当前目标只读", "muted", ["读取成功；你仍可搜索、选择与复制。", "修改控件禁用，权限详情解释如何获取权限。"], "查看权限说明"),
        ("S03", "名称不能为空", "red", ["字段错误与保存摘要同时显示。", "保留输入和光标；修正后才能进入保存确认。"], "定位错误字段"),
        ("S04", "发现可恢复草稿", "accent", ["草稿时间：2026-11-08 14:25", "目标若已变化，恢复后进入三快照差异。"], "恢复草稿  /  丢弃…"),
        ("S05", "目标被其他程序修改", "amber", ["当前有未保存修改；不会自动重载。", "无未保存内容时才允许自动刷新。"], "查看差异"),
        ("S06", "未能完成保存", "red", ["hosts 文件被占用；未报告写入成功。", "工作副本保留，显示备份位置与重试入口。"], "重试  /  查看详情"),
    ]
    for i, (sid, title, color, lines, action) in enumerate(cards):
        x = 40 + (i % 2) * 690
        y = 155 + (i // 2) * 246
        b.rect(x, y, 670, 224, "panel", "line", 12)
        b.badge(x + 22, y + 20, sid, color, 64)
        b.text(x + 22, y + 86, title, 22, color if color != "muted" else "ink", 700)
        for n, line in enumerate(lines):
            b.text(x + 22, y + 118 + n * 27, line, 14, "muted")
        b.button(x + 22, y + 169, 250, action, color == "accent")
    b.notes([("F13  草稿仅落到数据目录", "恢复 / 丢弃均需显式选择；保存失败不清理草稿。"),
             ("F10 / F16  错误分类", "加载、格式、权限、占用、备份、部分写入分别处理。"),
             ("F17 / T43–T46  可访问性", "状态同时有文字与图形；屏幕阅读器可读出原因。")])
    return b


def overview_board():
    b = Board("P00", "EnvHosts QML · 页面地图与写入边界", "目标：保留原生编辑器的完整能力，采用一致的 QML 页面、状态和命令模型。")
    b.rect(40, 151, 1360, 88, "panel", "line", 12)
    b.text(64, 187, "Win顺 启动器", 22, weight=700)
    b.text(64, 217, "env / hosts → 首条“进入交互式编辑界面”", 15, "muted")
    b.text(1374, 191, "各目标复用一个独立窗口", 18, "accent", 600, "end")
    b.text(1374, 218, "resident 协议 → C++ bridge → QML 页面", 14, "muted", anchor="end")
    cards = [("P01 / P08", "环境变量", "列表 · 原值 · 范围 · 明暗主题", "F02–F04 / F07"),
             ("P02", "分隔项编辑", "排序 · 多选 · 待删除 · 自动暂存", "F05–F06"),
             ("P03 / P04", "hosts", "映射条目 · 完整原文 · 格式保真", "F08–F10"),
             ("P05", "备份", "按范围预览差异 · 恢复工作副本", "F14"),
             ("P06", "保存 / 冲突", "变更确认 · 三快照差异 · 失败结果", "F11–F12"),
             ("P07", "状态 / 恢复", "空结果 · 只读 · 草稿 · 错误", "F13 / F16")]
    for i, (pid, title, detail, req) in enumerate(cards):
        x = 40 + (i % 3) * 460
        y = 268 + (i // 3) * 190
        b.rect(x, y, 440, 166, "panel", "line", 10)
        b.text(x + 22, y + 30, pid, 13, "accent", 700)
        b.text(x + 22, y + 67, title, 22, weight=700)
        b.text(x + 22, y + 99, detail, 14, "muted")
        b.text(x + 22, y + 141, req, 13, "accent")
    b.text(40, 677, "统一保存状态流", 22, weight=700)
    steps = [("01", "编辑 / 自动暂存", "只改内存工作副本"), ("02", "验证 / 确认", "展示目标与增改删"), ("03", "重读 / 比较", "变化则返回差异"), ("04", "备份 / 写入", "串行执行并读回"), ("05", "结果 / 草稿", "失败保留恢复信息")]
    for i, (num, title, detail) in enumerate(steps):
        x = 40 + i * 276
        b.rect(x, 702, 256, 118, "soft" if i < 3 else "panel", "line", 9)
        b.text(x + 16, 730, num, 13, "accent", 700)
        b.text(x + 16, 764, title, 19, weight=600)
        b.text(x + 16, 796, detail, 13, "muted")
        if i < 4:
            b.text(x + 260, 767, "→", 16, "accent")
    b.rect(40, 842, 1360, 60, "amberbg", radius=10)
    b.text(62, 879, "所有目标写入只从 F11 保存事务进入；草稿、导出和备份只写插件数据目录。测试默认使用隔离数据。", 17, "amber", 600)
    b.notes([("图 → 功能 → 测试", "P 页面编号 / F 功能要求 / T 验收用例双向追踪。"),
             ("本轮交付范围", "QML 核心编辑与紧凑修复已实现，验收见实现记录。"),
             ("后续开发顺序", "命令模型 → 编辑 → 保存恢复 → QML 交互 → 集成。")])
    return b


def flow_board():
    b = Board("P09", "状态转换 · 编辑与写入分开", "自动暂存只改变工作副本；错误、冲突和取消都有明确返回路径。")
    xs = [40, 393, 746, 1099]
    nodes = [("Clean", "与目标同步", "开始编辑进入 Dirty", "green"),
             ("Dirty", "有未保存修改", "普通切页与选择保持此状态", "amber"),
             ("Review", "审阅实际变更", "取消回 Dirty，保留修改", "accent"),
             ("Writing", "备份 / 写入 / 读回", "禁用重复保存，延后关闭", "accent")]
    for i, (state, title, detail, color) in enumerate(nodes):
        x = xs[i]
        b.rect(x, 218, 300, 145, "panel", "line", 10)
        b.badge(x + 18, 238, state, color, 110)
        b.text(x + 18, 303, title, 22, weight=700)
        b.text(x + 18, 339, detail, 13, "muted")
        if i < 3:
            b.text(x + 324, 296, "→", 24, "accent", anchor="middle")
            b.text(x + 324, 321, ["编辑", "保存", "确认"][i], 12, "muted", anchor="middle")
    b.line(1249, 218, 1249, 164, "green", 2)
    b.line(1249, 164, 190, 164, "green", 2)
    b.line(190, 164, 190, 216, "green", 2)
    b.text(190, 215, "▼", 13, "green", anchor="middle")
    b.text(720, 154, "全部写入并读回成功 → 更新基线、清理对应草稿", 14, "green", anchor="middle")
    branches = [("Draft", "恢复草稿", "显式恢复 → Dirty", "目标变化 → Conflict", "accent"),
                ("Invalid", "输入存在硬错误", "阻止保存，定位字段", "修正 → Dirty", "red"),
                ("Conflict", "目标被外部修改", "暂不处理 → Dirty + 提示", "覆盖 → 重新 Review", "amber"),
                ("Failed", "保存未完成", "保留工作副本和恢复信息", "重试 → 重新校验 / Review", "red")]
    for i,(state,title,line1,line2,color) in enumerate(branches):
        x=xs[i]
        b.rect(x, 469, 300, 162, "panel", "line", 10)
        b.badge(x + 18, 487, state, color, 110)
        b.text(x + 18, 550, title, 21, weight=700)
        b.text(x + 18, 582, line1, 13, "muted")
        b.text(x + 18, 608, line2, 13, "muted")
        if i:
            b.line(x+150,363,x+150,460,color,2)
            b.text(x+150,465,"▼",13,color,anchor="middle")
            b.text(x+165,419,["", "校验失败", "指纹变化", "任一步失败"][i],13,color)
    b.rect(40, 676, 1360, 219, "panel", "line", 10)
    b.text(64, 717, "三个交互约束", 22, weight=700)
    for i,(title,detail) in enumerate([
        ("切换不中断", "变量、筛选、页签、主题切换保留工作副本，不弹提交确认。"),
        ("撤销有边界", "输入中的 Esc 取消本次输入；已暂存的编辑通过统一命令历史撤销。"),
        ("失败不伪装成功", "部分写入进入失败状态，保留每项结果；不得标为已同步。")]):
        y=759+i*47
        b.text(64,y,title,15,"accent",700)
        b.text(214,y,detail,15,"muted")
    b.notes([("F05 / F15  自动暂存", "文本确认、移动和删除各形成可撤销命令。"),
             ("F11 / F12  写入边界", "确认 token 与目标指纹绑定；变化后重新审阅。"),
             ("原型验证", "三条任务可点击演示；实际 QML 与系统写入待实现。")])
    return b


def compact_board(page, kind="env", parts=False, dark=False):
    """Approved repair layout, drawn at logical size (not a runtime screenshot)."""
    b = Board(page, "紧凑列表 · " + ("hosts 状态" if kind == "hosts" else "分隔项" if parts else "环境变量"),
              "repair-1：范围按钮、连续列表、可调列宽、按列双击；控件按桌面密度统一。", dark)
    x, y, w = 160, 142, TOKENS["metrics"]["windowWidth"]
    margin = TOKENS["metrics"]["pageMargin"]
    h = TOKENS["metrics"]["rowHeight"]
    left, right = x + margin, x + w - margin
    b.rect(x, y, w, 760, "bg", "line", 5)
    b.text(left, y+30, "hosts 文件" if kind == "hosts" else "环境变量", 18, weight=600)
    b.button(right-290, y+12, 50, "撤销", h=26)
    b.button(right-234, y+12, 50, "重做", h=26)
    b.button(right-178, y+12, 66, "重载", h=26)
    b.button(right-106, y+12, 106, "保存全部修改", True, h=26)
    b.text(left, y+53, "修改暂存到工作副本 · 保存前可撤销", 12, "muted")
    b.button(left, y+66, 72, "条目" if kind == "hosts" else "用户变量", selected=True, h=26)
    b.button(left+78, y+66, 82, "全文" if kind == "hosts" else "系统变量", h=26)
    b.button(left+166, y+66, 58, "备份", h=26)
    b.rect(left, y+100, w-2*margin, 28, "field", "control", 3)
    b.text(left+6, y+119, "筛选当前范围 · Ctrl+F", 13, "muted")
    for i,label in enumerate(["新增", "删除所选", "恢复所选"] + (["启用", "停用"] if kind == "hosts" else [])):
        b.button(left+i*78, y+136, 72, label, h=26)
    top=y+170
    b.rect(left,top,w-2*margin,218,"panel","line",3)
    b.rect(left+1,top+1,w-2*margin-2,28,"field",radius=0)
    if kind=="hosts":
        columns=[(32,"启停"),(122,"IP"),(272,"主机名"),(720,"行尾注释"),(982,"修改状态")]
        rows=[("−", "127.0.0.1", "old.test", "旧映射", "− 待删除"),("● 启用","::1","localhost","IPv6","—"),("Ⅱ 停用","192.0.2.20","api.test","本地接口","—")]
    else:
        columns=[(32,"变量名"),(214,"值"),(832,"类型"),(982,"修改状态")]
        rows=[("Path",r"%USERPROFILE%\bin;C:\Tools\bin","REG_EXPAND_SZ","● 已修改"),("JAVA_HOME",r"C:\SDK\jdk-21","REG_SZ","—"),("DEV_MODE","development","REG_SZ","—")]
    for offset,label in columns:
        b.text(left+offset+7,top+19,label,12,"muted")
        b.line(left+offset-4,top+6,left+offset-4,top+22,"control")
    for i,row in enumerate(rows):
        ry=top+28+i*h
        fill="redbg" if kind=="hosts" and i==0 else "soft" if i==0 else "field" if kind=="hosts" and i==2 else "panel"
        b.rect(left+1,ry,w-2*margin-2,h,fill,"accent" if i==0 else None,0)
        b.rect(left+9,ry+7,14,14,"accent" if i==0 else "panel","control",2)
        if kind=="hosts" and i==0:b.rect(left+1,ry,3,h,"red",radius=0)
        for j,((offset,_),value) in enumerate(zip(columns,row)):
            color="red" if kind=="hosts" and i==0 else "green" if kind=="hosts" and j==0 and i==1 else "amber" if kind=="hosts" and j==0 and i==2 else "ink"
            b.text(left+offset+7,ry+19,value,13,color,mono=j==1 or (kind=="hosts" and j==2))
    b.text(left,top+242,"独立注释与空行只在全文显示；停用映射仍在列表。" if kind=="hosts" else "双击：名称→名称输入；值→原值；类型→选择器。拖动列边界，双击边界适应内容。",12,"muted")
    if kind=="hosts":
        for fx, fw, label, value in [(left,240,"IP","127.0.0.1"),(left+256,410,"主机名","old.test"),(left+682,414,"行尾注释","旧映射")]:
            b.text(fx,top+275,label,12,"muted")
            b.rect(fx,top+283,fw,28,"field","control",3);b.text(fx+6,top+302,value,13)
    else:
        b.text(left,top+275,"变量名",12,"muted")
        b.rect(left,top+283,770-24,28,"field","control",3);b.text(left+6,top+302,"Path",13)
        b.button(left+758,top+283,150,"REG_EXPAND_SZ  ⌄",h=28)
        b.button(right-156,top+283,74,"拆分项",selected=parts,h=28)
        b.button(right-76,top+283,76,"原始值",selected=not parts,h=28)
        b.text(left,top+333,"37 / 32767 UTF-16",12,"muted")
        if parts:
            for i,label in enumerate(["新增项","删除所选","恢复所选","↑","↓"]):b.button(left+i*80,top+343,74,label,h=26)
            b.rect(left,top+377,w-2*margin,184,"panel","line",3)
            b.text(left+40,top+397,"#     值",12,"muted")
            for i,value in enumerate([r"%USERPROFILE%\bin",r"C:\Tools\bin",r"C:\OldSDK\bin",r"D:\SDK\bin"]):
                ry=top+405+i*h;b.rect(left+1,ry,w-2*margin-2,h,"redbg" if i==2 else "greenbg" if i==3 else "soft" if i<2 else "panel","accent" if i<2 else None,0)
                b.rect(left+9,ry+7,14,14,"accent" if i<2 else "panel","control",2);b.text(left+40,ry+19,i+1,12,"muted");b.text(left+68,ry+19,value,13,"red" if i==2 else "green" if i==3 else "ink",mono=True)
                if i==2:b.text(right-84,ry+19,"− 待删除",12,"red")
                if i==3:b.text(right-84,ry+19,"+ 新增",12,"green")
        else:
            b.rect(left,top+348,w-2*margin,160,"field","control",3);b.text(left+8,top+372,r"%USERPROFILE%\bin;C:\Tools\bin",13,mono=True)
    b.text(left,y+742,"28 DIP 行高 · 26 DIP 按钮 · 28 DIP 输入 · 12 DIP 页边距",12,"muted")
    b.notes([("紧凑共享控件","覆盖 Qt Basic 的默认 padding / 背景 / indicator 尺寸。"),("状态与选择","待删除背景优先，多选描边独立叠加。"),("真实验收","此图为设计示意；Qt 截图与自动化证据见实现记录。")])
    return b


def gallery(boards):
    nav = "\n".join(f'<a href="#{b.page}">{b.page} {escape(b.title)}</a>' for _, b in boards)
    sections = "\n".join(f'<section id="{b.page}"><h2>{b.page} · {escape(b.title)}</h2><a href="{name}.svg"><img src="{name}.svg" alt="{escape(b.title)}" loading="lazy"/></a></section>' for name, b in boards)
    return f'''<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EnvHosts QML · 设计评审</title>
<style>html{{scroll-behavior:smooth}}body{{margin:0;background:#edf1f5;color:#16273b;font:15px "Segoe UI","Microsoft YaHei UI",sans-serif}}header{{padding:28px 32px;background:white;border-bottom:1px solid #cbd5e1}}h1{{margin:0 0 12px}}p{{color:#536578}}nav{{display:flex;gap:10px;flex-wrap:wrap}}a{{color:#185abd}}nav a{{padding:8px 12px;background:#e9f1ff;border-radius:7px;text-decoration:none}}main{{max-width:1440px;margin:auto;padding:24px}}section{{scroll-margin-top:20px;margin-bottom:48px}}h2{{font-size:18px}}img{{display:block;width:100%;height:auto;border-radius:12px;box-shadow:0 8px 30px #16273b12}}</style>
<header><h1>EnvHosts QML · 设计评审</h1><p>2026-11-08 · repair-1 设计与实现对照 · {len(boards)} 张 SVG · 所有数据为合成示例。点击图可打开原始 SVG。</p><p><a href="../repair-prototype/index.html">紧凑列表修复交互演示 →</a> · <a href="../IMPLEMENTATION.md">实际实现与验证记录</a></p><nav>{nav}</nav></header>
<main>{sections}</main></html>
'''


def token_css():
    aliases = {"ink": "text", "greenbg": "green-bg", "amberbg": "amber-bg",
               "redbg": "red-bg", "onAccent": "on-accent", "accentHover": "accent-hover"}
    lines = ["/* Generated from ../design-tokens.json; edit the source, not this file. */"]
    for theme, selector in (("light", ":root"), ("dark", "body.dark")):
        values = ";".join(f"--{aliases.get(key, key)}:{value}" for key, value in TOKENS["colors"][theme].items())
        lines.append(f"{selector}{{color-scheme:{theme};{values}}}")
    return "\n".join(lines) + "\n"


def token_qml():
    lines = ["// Generated from docs/qml/design-tokens.json. Do not edit.", "import QtQuick", "QtObject {", "    property bool dark: false"]
    for key in LIGHT:
        lines.append(f'    readonly property color {key}: dark ? "{DARK[key]}" : "{LIGHT[key]}"')
    for key, value in TOKENS["metrics"].items():
        lines.append(f"    readonly property int {key}: {value}")
    lines += ["}", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Check generated assets without changing files")
    args = parser.parse_args()
    boards = [("00-overview", overview_board()), ("01-environment", compact_board("P01")),
              ("02-split-items", compact_board("P02",parts=True)), ("03-hosts-entries", compact_board("P03","hosts")),
              ("04-hosts-raw", hosts_board(True)), ("05-backups", backup_board()),
              ("06-save-conflict", review_board()), ("07-states-recovery", states_board()),
              ("08-environment-dark", compact_board("P08",parts=True,dark=True)), ("09-state-flow", flow_board())]
    files = {f"{name}.svg": b.done() for name, b in boards}
    files["index.html"] = gallery(boards)
    files["../prototype/tokens.css"] = token_css()
    files["../../../qt/qml/DesignTokens.qml"] = token_qml()
    stale = []
    if not args.check:
        OUT.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        path = OUT / name
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                stale.append(name)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    if stale:
        raise SystemExit("Stale design assets: " + ", ".join(stale))
    print(f"{'Verified' if args.check else 'Generated'} {len(files)} design assets")


if __name__ == "__main__":
    main()
