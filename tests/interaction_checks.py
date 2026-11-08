"""Real-window checks for the five native-editor interaction regressions.

Only the driver's isolated registry keys, hosts and data directory are used.
Geometry/state messages are read-only; every change goes through ordinary UI.
"""
from pathlib import Path
from types import SimpleNamespace


def run(context):
    g = SimpleNamespace(**context)
    h = g.env_hwnd
    g.check(bool(h), "interaction editor opened")
    if not h:
        return
    parts, raw, inline = [g.child_by_id(h, i) for i in (10, 4, 11)]

    def key(vk, **mods):
        g.post_key(h, vk, **mods)

    def probe(which):
        return g.send(h, g.WM_APP_GETLAYOUT, which)

    def rect(which, index=0):
        return g.layout_rect(h, which, index)

    def click(r):
        g.click_at(h, *g.center(r))
        g.time.sleep(0.15)

    def values():
        return [g.lb_text(parts, i) for i in range(g.send(parts, g.LB_GETCOUNT))]

    def caret():
        return g.send(parts, g.LB_GETCURSEL)

    def staged():
        return (g.send(h, g.WM_APP_GETSTATE) >> 51) & 31

    def focus(ref):
        g.focus_row(h, ref)
        g.time.sleep(0.15)

    def choose(index):
        g.send(parts, g.LB_SETTOPINDEX, index)
        top = g.send(parts, g.LB_GETTOPINDEX)
        y = (index - top) * g.send(parts, g.LB_GETITEMHEIGHT) + 8
        g.user32.PostMessageW(parts, g.WM_LBUTTONDOWN, g.MK_LBUTTON, g.lparam(95, y))
        g.user32.PostMessageW(parts, g.WM_LBUTTONUP, 0, g.lparam(95, y))
        g.time.sleep(0.15)

    g.check(probe(17) == 3, "backup count loads before opening Backups", probe(17))
    key(0x09, ctrl=True)
    key(0x09, ctrl=True)
    click(rect(21, 1))
    g.check(probe(18) == 2, "User backup scope contains both user snapshots", probe(18))
    click(rect(21, 2))
    g.check(probe(18) == 1, "System backup scope contains only the system snapshot", probe(18))
    g.check(probe(17) == 3, "backup tab count is independent of scope", probe(17))
    key(0x09, ctrl=True)
    g.check(probe(17) == 3, "backup total survives returning to User")

    focus("user/Path")
    key(ord("R"), ctrl=True)
    choose(0)
    original = list(g.PATH_SEGS)
    key(g.VK_DELETE)
    g.check(staged() == 1 and values() == original, "delete remains a visible candidate")
    g.check(probe(25) == 1, "Path row displays Pending before Commit")
    g.check(not probe(19) and probe(22) == len(g.PATH_SAMPLE), "pending deletion does not modify the work model")
    focus("user/LANG")
    g.check(values() == ["C"], "other variable stays unchanged")
    g.check(probe(25) == 0 and probe(29) == 0, "unchanged single-item variable has no status or move action")
    focus("system/Path")
    g.check(probe(25) == 0, "same variable name in System has independent status")
    focus("user/Path")
    g.check(staged() == 1 and values() == original, "returning to variable preserves pending deletion")
    g.check(probe(25) == 1, "returning to User restores its pending row indicator")
    key(0x09, ctrl=True)
    key(0x09, ctrl=True)
    key(0x09, ctrl=True)
    focus("user/Path")
    g.check(staged() == 1 and not probe(19), "tab round trip preserves candidates without committing")
    filt = g.child_by_id(h, 1)
    g.set_text(h, filt, 1, "LANG")
    g.set_text(h, filt, 1, "")
    focus("user/Path")
    g.check(staged() == 1 and values() == original and not probe(19), "filter round trip preserves candidates without committing")
    key(ord("Z"), ctrl=True)
    g.check(staged() == 0 and values() == original, "undo cancels pending deletion locally")
    g.check(probe(25) == 0, "undo back to source clears the pending row indicator")
    key(ord("Y"), ctrl=True)
    g.check(staged() == 1 and values() == original and not probe(19), "redo restores pending marker without committing")
    click(rect(4))
    g.check(values() == original[1:] and probe(19), "Commit explicitly applies the candidate to the model")
    g.check(probe(25) == 2, "Commit changes row indicator to Unsaved")
    choose(0)
    key(g.VK_DELETE)
    g.check(probe(25) == 3, "new candidate on a modified row displays Pending and Unsaved together")
    g.shot(h, "row_pending_unsaved")
    key(ord("Z"), ctrl=True)
    g.check(probe(25) == 2, "candidate undo retains the existing Unsaved state")
    key(ord("Z"), ctrl=True)
    g.check(values() == original and staged() == 0 and not probe(19), "one model undo reverses Commit")
    g.check(probe(25) == 0, "model undo clears the final row indicator")

    choose(0)
    g.check(probe(29) == 2, "first item disables Up and enables Down")
    click(rect(12))
    g.check(values() == original and caret() == 0 and probe(25) == 0, "disabled Up is a no-op")
    # Real list-key dispatch, followed by WM_CHAR as TranslateMessage produces.
    g.send(parts, g.WM_KEYDOWN, ord("J"))
    g.send(parts, 0x0102, ord("j"))
    g.check(caret() == 1, "native j moves down without type-to-select")
    g.send(parts, g.WM_KEYDOWN, ord("K"))
    g.check(caret() == 0, "native k moves up")
    key(ord("J"), ctrl=True)
    g.check(caret() == 1, "Ctrl+J moves down")
    key(ord("K"), ctrl=True)
    g.check(caret() == 0, "Ctrl+K moves up")
    key(ord("G"), shift=True)
    g.check(caret() == len(original) - 1, "G moves to last split item")
    g.check(probe(29) == 1, "last item enables Up and disables Down")
    g.send(parts, g.WM_KEYDOWN, ord("G"))
    g.send(parts, g.WM_KEYDOWN, ord("G"))
    g.check(caret() == 0, "native gg moves to first split item")
    g.send(parts, g.WM_KEYDOWN, ord("D"))
    g.check(staged() == 0, "single d does not delete")
    g.send(parts, g.WM_KEYDOWN, ord("D"))
    g.check(staged() == 1 and not probe(19), "native dd stages selected item")
    key(ord("Z"), ctrl=True)
    key(ord("J"), alt=True)
    g.check(values()[:2] == original[1::-1] and caret() == 1, "Alt+J moves item down", (values()[:3], caret()))
    key(ord("K"), alt=True)
    g.check(values() == original and caret() == 0, "Alt+K moves item up")
    click(rect(13))
    g.check(values()[:2] == original[1::-1], "visible Down button reorders item")
    g.check(probe(25) == 1, "reorder alone sets the pending row indicator")
    g.send(parts, g.WM_KEYDOWN, ord("K"))
    g.check(caret() == 0, "footer move returns keyboard focus to the item list")
    choose(1)
    click(rect(12))
    g.check(values() == original, "visible Up button reorders item")
    g.check(probe(25) == 0, "moving back to original order clears pending status")

    key(ord("N"), ctrl=True)
    g.set_text(h, inline, 11, r"C:\sample\pending")
    g.send(inline, g.WM_KEYDOWN, 0x0D)
    pending = values()
    focus("user/LANG")
    focus("user/Path")
    g.check(values() == pending and not probe(19), "new item and its edit remain candidates after navigation")
    g.check(probe(25) == 1, "added item shows a pending indicator on its variable")
    choose(1)
    key(g.VK_DELETE)
    g.check(values() == original and staged() == 0, "new item retains its identity after navigation", (values()[:4], staged(), caret()))
    g.check(probe(25) == 0, "removing an uncommitted new item clears pending status")

    # Pending changes in two variables must survive draft persistence and Save.
    choose(0)
    key(g.VK_DELETE)
    focus("user/LANG")
    choose(0)
    key(0x0D)
    g.set_text(h, inline, 11, "pending locale")
    g.send(inline, g.WM_KEYDOWN, 0x0D)
    g.check(probe(25) == 1, "editing alone shows Pending on the variable row")
    g.send(h, 0x0113, 1)  # ordinary draft timer
    draft = g.json.loads((Path(g.DATA) / "drafts/env.json").read_text(encoding="utf-8"))
    entries = {entry["ref"]: entry for entry in draft.get("itemCandidates", [])}
    g.check(set(entries) == {"user/Path", "user/LANG"}, "draft keeps candidates for both variables", list(entries))
    g.check(not probe(19), "draft persistence never commits candidates")
    g.check(g.reg_value("Path") == g.PATH_SAMPLE and g.reg_value("LANG") == "C", "navigation never writes real fixture values")

    for language, theme in (("zh", "light"), ("en", "dark")):
        g.call("appearance", {"locale": language, "theme": theme})
        g.time.sleep(0.2)
        for width in (980, 660):
            g.user32.SetWindowPos(h, None, 30, 30, width, 650, 0x0040)
            g.time.sleep(0.2)
            cw, ch = g.client_size(h)
            boxes = [rect(15, i) for i in range(probe(28))]
            text = [rect(16, i) for i in range(probe(28))]
            fits = all(0 <= r[0] < r[2] <= cw and 0 <= r[1] < r[3] <= ch
                       and r[2] - r[0] >= t[2] + 12 and r[3] - r[1] >= t[3] + 6
                       for r, t in zip(boxes, text))
            overlaps = any(a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]
                           for i, a in enumerate(boxes) for b in boxes[i+1:])
            g.check(fits and not overlaps, "%s %d: all footer labels fit without overlaps" % (language, width), boxes)
            _, list_rect = g.child_client_rect(h, 10)
            footer = rect(14)
            g.check(all(list_rect[3] <= rect(i)[1] and footer[1] <= rect(i)[1] < rect(i)[3] <= footer[3] for i in (12, 13)), "%s %d: Up/Down share the footer below the list" % (language, width))
            group = rect(30)
            g.check(group[0] < group[2] <= rect(12)[0] and group[1] == rect(12)[1] == rect(13)[1], "%s %d: Items group stays together" % (language, width))
            label, column = rect(26), rect(27)
            g.check(column[2] - column[0] >= label[2] + 10, "%s %d: Pending row label fits its status column" % (language, width))
            g.check(list_rect[1] - rect(23)[1] == 96, "%s %d: item list reclaims the old movement toolbar row" % (language, width))
            g.shot(h, "interaction_%s_%d" % (language, width))

    key(ord("R"), ctrl=True)
    g.check(probe(28) == 7 and rect(12) == (0, 0, 0, 0), "raw mode hides the item footer group")
    g.check(probe(25) == 1, "raw mode retains the variable pending indicator")
    key(ord("R"), ctrl=True)

    g.call("appearance", {"locale": "zh", "theme": "light"})
    # Reopen the same editor through the protocol and restore its real draft.
    g.user32.PostMessageW(h, 0x0010, 0, 0)
    dlg = g.find_dialog(3)
    g.check(bool(dlg), "closing pending candidates asks before leaving")
    if dlg:
        g.press_dialog_button(dlg, g.IDNO)
    g.time.sleep(0.3)
    focus("user/Path")
    dlg = g.find_dialog(3)
    g.check(bool(dlg), "reopening offers the candidate draft")
    if dlg:
        g.press_dialog_button(dlg, g.IDYES)
    g.time.sleep(0.3)
    h = next((hwnd for hwnd, title in g.windows_of(g.proc.pid) if "环境" in title or "Environment" in title), None)
    g.check(bool(h), "editor reopened after draft restore")
    if not h:
        return
    parts, raw, inline = [g.child_by_id(h, i) for i in (10, 4, 11)]
    focus("user/Path")
    key(ord("R"), ctrl=True)
    g.check(staged() == 1 and values() == original and not probe(19), "restored draft preserves pending flags and uncommitted model")
    g.check(probe(25) == 1, "draft recovery restores the variable pending indicator")
    choose(0)
    key(ord("Z"), ctrl=True)
    g.check(staged() == 0 and values() == original and not probe(19), "restored candidates can be undone without changing the model")
    key(ord("Y"), ctrl=True)
    g.check(staged() == 1 and not probe(19), "restored candidate undo can be redone")
    focus("user/LANG")
    g.check(values() == ["pending locale"], "restored draft retains the other variable's candidate")
    key(ord("S"), ctrl=True)
    dlg = g.find_dialog(3)
    g.check(bool(dlg), "explicit Save asks for confirmation")
    if dlg:
        g.press_dialog_button(dlg, g.IDYES)
        g.time.sleep(0.5)
    g.check(g.reg_value("Path") == ";".join(original[1:]) and g.reg_value("LANG") == "pending locale", "explicit Save applies both variable candidates", (g.reg_value("Path"), g.reg_value("LANG")))
    g.check(probe(17) == 4, "backup count refreshes immediately after Save", probe(17))
    g.check(probe(25) == 0, "Save clears the current variable status")
    focus("user/Path")
    g.check(probe(25) == 0, "Save clears the other variable status")
    key(ord("U"), ctrl=True)
    dlg = g.find_dialog(3)
    g.check(bool(dlg), "whole-variable deletion still asks for confirmation")
    if dlg:
        g.press_dialog_button(dlg, g.IDYES)
        g.time.sleep(0.3)
    g.check(probe(25) == 10 and probe(29) == 0, "deleted variable shows Unsaved deletion and disables item movement")
    key(ord("Z"), ctrl=True)
    g.check(probe(25) == 0, "undoing whole-variable deletion clears its status")
    click(rect(15, 0))
    g.set_text(h, g.child_by_id(h, 2), 2, "WS_ADDED_STATUS")
    g.check(probe(25) == 6, "new variable displays New and Unsaved")
    choose(0)
    key(0x0D)
    g.set_text(h, inline, 11, "pending new item")
    g.send(inline, g.WM_KEYDOWN, 0x0D)
    g.check(probe(25) == 7, "new variable can also display Pending without committing its item")
    for locale in ("zh", "en"):
        g.call("appearance", {"locale": locale, "theme": "dark" if locale == "en" else "light"})
        g.time.sleep(0.2)
        label, column = rect(26), rect(27)
        g.check(column[2] - column[0] >= label[2] + 10, locale + ": combined Pending/New/Unsaved label fits")
    g.shot(h, "new_pending_en")
    filt = g.child_by_id(h, 1)
    g.set_text(h, filt, 1, "WS_NO_MATCH_PENDING_STATUS")
    g.check(probe(29) == 0, "empty filtered list disables both item movement actions")
