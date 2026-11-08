"""Focused real-window regressions, run by gui_preview.py --split-state-check.

All edits remain in the isolated preview model/registry. Expected lists are
independent fixture values; no production state-manipulation hooks are used.
"""
from types import SimpleNamespace


def run(context):
    g = SimpleNamespace(**context)
    h = g.env_hwnd
    g.check(h is not None, "split-state editor opened")
    if not h:
        return
    parts = g.child_by_id(h, 10)
    raw = g.child_by_id(h, 4)
    inline = g.child_by_id(h, 11)
    expected = [r"C:\sample\alpha", r"C:\sample\beta", r"C:\sample\gamma"]

    def key(vk, **mods):
        g.post_key(h, vk, **mods)

    def items():
        return [g.lb_text(parts, i) for i in range(g.send(parts, g.LB_GETCOUNT))]

    def staged():
        return (g.send(h, g.WM_APP_GETSTATE) >> 51) & 31

    def mode(want):
        if bool(g.user32.IsWindowVisible(parts)) != want:
            key(ord("R"), ctrl=True)

    def choose(index, mods=0):
        y = index * g.send(parts, g.LB_GETITEMHEIGHT) + 8
        g.user32.PostMessageW(parts, g.WM_LBUTTONDOWN,
                             g.MK_LBUTTON | mods, g.lparam(95, y))
        g.user32.PostMessageW(parts, g.WM_LBUTTONUP, mods, g.lparam(95, y))
        g.time.sleep(0.2)

    def reset():
        # Finish this test's candidate explicitly before installing a fresh fixture.
        g.click_at(h, *g.center(g.layout_rect(h, 4)))
        mode(False)
        g.focus_row(h, "user/Path")
        g.time.sleep(0.3)
        g.set_text(h, raw, 4, ";".join(expected))
        g.time.sleep(0.5)  # end the text-edit undo transaction
        mode(True)
        choose(0)

    reset()
    key(g.VK_DELETE)
    g.check(staged() == 1 and items() == expected,
            "deletion keeps the staged row visible")
    key(ord("Z"), ctrl=True)
    g.check(items() == expected and staged() == 0,
            "undo before Commit restores all split items", items())
    g.check(g.get_text(raw) == ";".join(expected),
            "undo restores the model value without reapplying stale controls")
    key(ord("Y"), ctrl=True)
    g.check(items() == expected and staged() == 1,
            "redo restores exactly one pending deletion", items())

    reset()
    choose(1)
    key(g.VK_DELETE)
    g.focus_row(h, "user/Path")
    g.time.sleep(0.4)
    g.check(items() == expected and staged() == 1,
            "reopening the same row preserves its pending deletion", items())
    g.check(g.get_text(raw) == ";".join([expected[0], expected[2]]),
            "raw preview excludes the pending deletion without committing it")

    reset()
    key(g.VK_DELETE)
    g.focus_row(h, "user/LANG")
    g.time.sleep(0.4)
    g.check(items() == ["C"], "changing rows does not delete the new row's item", items())
    mode(False)
    g.check(g.get_text(raw) == "C", "switching to raw preserves the other variable")

    reset()
    key(g.VK_DELETE)
    # Reinterpreting item boundaries must not silently accept pending deletes.
    g.user32.PostMessageW(h, g.WM_COMMAND, 201, 0)
    g.time.sleep(0.4)
    g.check(items() == expected and staged() == 1,
            "delimiter change preserves pending deletions until explicitly committed", items())
    g.click_at(h, *g.center(g.layout_rect(h, 4)))
    g.user32.PostMessageW(h, g.WM_COMMAND, 201, 0)
    g.time.sleep(0.4)
    g.user32.PostMessageW(h, g.WM_COMMAND, 200, 0)
    g.time.sleep(0.4)
    g.check(items() == expected[1:], "delimiter round trip preserves committed remaining items", items())

    reset()
    key(ord("N"), ctrl=True)
    g.check(g.user32.IsWindowVisible(inline), "Ctrl+N opens the new item's inline editor")
    g.send(inline, g.WM_KEYDOWN, 0x1B)  # real child key dispatch
    choose(1)
    key(ord("K"), alt=True)  # move the new item above alpha
    g.check(items() == [""] + expected, "new item can move before an existing item", items())
    key(g.VK_DELETE)
    g.check(items() == expected and staged() == 0,
            "deleting a moved new item removes it immediately", items())

    reset()
    key(ord("N"), ctrl=True)
    g.send(inline, g.WM_KEYDOWN, 0x1B)
    choose(0)
    choose(1, 0x0008)  # Ctrl+click: existing alpha and the new empty item
    key(g.VK_DELETE)
    g.check(items() == expected and staged() == 1,
            "mixed deletion removes the new item and keeps the existing item staged", items())
    g.shot(h, "split_mixed_delete")
    choose(0)
    key(g.VK_DELETE)
    g.check(items() == expected and staged() == 0,
            "mixed batch's existing item can still be restored", items())

    reset()
    key(g.VK_DELETE)
    # Filter rebuilds visible indices before selecting a row.
    filt = g.child_by_id(h, 1)
    g.set_text(h, filt, 1, "LANG")
    g.check(g.get_text(g.child_by_id(h, 2)) == "LANG" and items() == ["C"],
            "filtering after deletion preserves the newly selected variable", items())
    mode(False)
    g.check(g.get_text(raw) == "C", "filtered variable retains its value")
    g.check(g.reg_value("Path") == g.PATH_SAMPLE and g.reg_value("LANG") == "C",
            "split-state checks never write the registry before Save")
