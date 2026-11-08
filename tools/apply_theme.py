#!/usr/bin/env python3
"""One-shot migration: move editor_win.h's inline colours into the theme.

Until v0.7.0 editor_win.h painted with inline `m_dark ? RGB(a) : RGB(b)`
pairs - 81 of them. This script rewrites each of them to a lookup into
editor_theme.h, and appends any role that does not exist yet.

The mapping below is the human-reviewed part: each entry says which theme
role a colour pair belongs to. Run once; afterwards the theme file is the
only place a colour lives.

Usage:
    python tools/apply_theme.py            # apply (idempotent)
    python tools/apply_theme.py --check    # report remaining inline colours
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "editor_win.h")
THEME = os.path.join(ROOT, "editor_theme.h")

# (light_rgb, dark_rgb) -> theme role name.
# Roles that already exist in editor_theme.h are reused; the rest are added.
MAP = {
    # surfaces / borders
    ((238, 241, 246), (42, 46, 53)): "kSubHeadBg",
    ((238, 241, 246), (36, 40, 46)): "kSectionBg",
    ((200, 206, 214), (70, 76, 84)): "kBorder",
    ((230, 233, 238), (52, 56, 62)): "kBorderSoft",
    ((206, 212, 220), (70, 76, 84)): "kBorder",
    ((246, 248, 251), (30, 33, 38)): "kPanel",
    ((250, 251, 253), (30, 33, 39)): "kDiffBg",
    ((241, 239, 232), (44, 48, 54)): "kHeadBg",
    ((241, 239, 232), (40, 44, 50)): "kHeadBg",
    ((236, 239, 244), (38, 41, 46)): "kStatusBg",
    ((232, 236, 242), (36, 40, 46)): "kStatusRow1Bg",
    ((241, 243, 247), (30, 33, 38)): "kStatusRow2Bg",
    # selection
    ((38, 48, 62), (230, 241, 251)): None,   # placeholder, fixed below
    ((210, 228, 250), (38, 42, 48)): "kSelBg",
    ((228, 238, 252), (38, 42, 48)): "kPreviewSelBg",
    ((228, 238, 252), (48, 60, 76)): "kMultiSelBg",
    ((55, 138, 221), (120, 180, 250)): "kSelBar",
    ((60, 130, 215), (120, 180, 250)): "kFocusBar",
    ((228, 232, 238), (48, 52, 58)): "kTabBg",
    ((230, 241, 251), (36, 48, 62)): "kStatusSelBg",
    ((230, 241, 251), (32, 48, 68)): "kChipInfoBg",
    ((230, 241, 251), (44, 60, 80)): "kFocusRing",
    ((252, 235, 235), (52, 34, 36)): "kPartAddBg",
    ((252, 235, 235), (72, 36, 38)): "kChipDangerBg",
    # text
    ((32, 39, 51), (232, 234, 237)): "kText",
    ((110, 110, 110), (130, 136, 144)): "kTextDim",
    ((110, 116, 126), (150, 156, 166)): "kTextDim2",
    ((130, 130, 130), (130, 136, 144)): "kMutedText",
    ((24, 95, 165), (130, 180, 240)): "kAddedText",
    ((24, 95, 165), (150, 195, 250)): "kChipInfoFg",
    ((24, 95, 165), (180, 215, 255)): "kAccentText",
    ((163, 45, 45), (236, 150, 150)): "kDeletedText",
    ((163, 45, 45), (240, 150, 150)): "kChipDangerFg",
    ((170, 50, 50), (230, 130, 130)): "kDeleted",
    ((170, 46, 46), (244, 150, 150)): "kOverText",
    ((170, 60, 60), (220, 120, 120)): "kRemovedText",
    ((176, 44, 44), (240, 120, 120)): "kDiffRed",
    ((28, 92, 168), (120, 180, 250)): "kDiffBlue",
    ((12, 68, 124), (180, 215, 255)): "kChipTextStrong",
    ((150, 96, 12), (230, 186, 100)): "kWarnText",
    ((150, 98, 10), (235, 190, 100)): "kModified",
    ((178, 42, 42), (244, 140, 140)): "kOverText",
    ((202, 138, 30), (170, 130, 40)): "kElevText",
    ((160, 100, 20), (230, 170, 90)): "kDiffValue",
    ((30, 120, 50), (120, 200, 130)): "kDiffSame",
    ((170, 50, 50), (230, 120, 120)): "kDiffExtra",
    # misc
    ((68, 68, 65), (220, 224, 230)): "kChipBorder",
    ((190, 196, 204), (90, 96, 104)): "kChipElevBg",
    ((236, 238, 241), (40, 43, 48)): "kChipOffBg",
    ((220, 222, 226), (55, 58, 64)): "kChipEdge",
    ((255, 255, 255), (48, 52, 58)): "kChipBg",
    ((160, 168, 180), (96, 104, 116)): "kGridPen",
    ((133, 183, 235), (80, 118, 168)): "kPillEdge",
    ((242, 242, 242), (42, 45, 50)): "kBtnBg",
    ((215, 228, 244), (60, 66, 76)): "kBtnActiveBg",
    ((225, 232, 241), (48, 52, 58)): "kBtnHoverBg",
    ((176, 188, 202), (90, 98, 110)): "kBtnEdge",
    ((220, 222, 226), (60, 64, 70)): "kBtnEdgeDim",
    ((62, 132, 214), (120, 180, 250)): "kAccentDeep",
    ((226, 75, 74), (226, 100, 100)): "kDangerSoft",
    ((196, 62, 62), (232, 104, 104)): "kDanger",
}

# The pair whose two colours are swapped relative to the table above: the
# light value is the bright one. Fix it explicitly so it is not a guess.
MAP[((230, 241, 251), (38, 48, 62))] = "kPartSelBg"

# Roles that must exist in the theme after the migration, with their values.
NEW_ROLES = {
    "kSubHeadBg": ((238, 241, 246), (42, 46, 53)),
    "kSectionBg": ((238, 241, 246), (36, 40, 46)),
    "kDiffBg": ((250, 251, 253), (30, 33, 39)),
    "kStatusBg": ((236, 239, 244), (38, 41, 46)),
    "kStatusRow1Bg": ((232, 236, 242), (36, 40, 46)),
    "kStatusRow2Bg": ((241, 243, 247), (30, 33, 38)),
    "kPreviewSelBg": ((228, 238, 252), (38, 42, 48)),
    "kMultiSelBg": ((228, 238, 252), (48, 60, 76)),
    "kFocusBar": ((60, 130, 215), (120, 180, 250)),
    "kTabBg": ((228, 232, 238), (48, 52, 58)),
    "kStatusSelBg": ((230, 241, 251), (36, 48, 62)),
    "kChipInfoBg": ((230, 241, 251), (32, 48, 68)),
    "kPartSelBg": ((230, 241, 251), (38, 48, 62)),
    "kPartAddBg": ((252, 235, 235), (52, 34, 36)),
    "kChipDangerBg": ((252, 235, 235), (72, 36, 38)),
    "kTextDim2": ((110, 116, 126), (150, 156, 166)),
    "kMutedText": ((130, 130, 130), (130, 136, 144)),
    "kAddedText": ((24, 95, 165), (130, 180, 240)),
    "kChipInfoFg": ((24, 95, 165), (150, 195, 250)),
    "kAccentText": ((24, 95, 165), (180, 215, 255)),
    "kDeletedText": ((163, 45, 45), (236, 150, 150)),
    "kChipDangerFg": ((163, 45, 45), (240, 150, 150)),
    "kOverText": ((170, 46, 46), (244, 150, 150)),
    "kRemovedText": ((170, 60, 60), (220, 120, 120)),
    "kDiffRed": ((176, 44, 44), (240, 120, 120)),
    "kDiffBlue": ((28, 92, 168), (120, 180, 250)),
    "kChipTextStrong": ((12, 68, 124), (180, 215, 255)),
    "kWarnText": ((150, 96, 12), (230, 186, 100)),
    "kElevText": ((202, 138, 30), (170, 130, 40)),
    "kChipBorder": ((68, 68, 65), (220, 224, 230)),
    "kChipElevBg": ((190, 196, 204), (90, 96, 104)),
    "kChipOffBg": ((236, 238, 241), (40, 43, 48)),
    "kChipEdge": ((220, 222, 226), (55, 58, 64)),
    "kChipBg": ((255, 255, 255), (48, 52, 58)),
    "kGridPen": ((160, 168, 180), (96, 104, 116)),
    "kPillEdge": ((133, 183, 235), (80, 118, 168)),
    "kBtnBg": ((242, 242, 242), (42, 45, 50)),
    "kBtnActiveBg": ((215, 228, 244), (60, 66, 76)),
    "kBtnHoverBg": ((225, 232, 241), (48, 52, 58)),
    "kBtnEdge": ((176, 188, 202), (90, 98, 110)),
    "kBtnEdgeDim": ((220, 222, 226), (60, 64, 70)),
    "kDangerSoft": ((226, 75, 74), (226, 100, 100)),
}

PAIR_RE = re.compile(
    r"m_dark\s*\?\s*RGB\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\s*:\s*RGB\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)"
)


def main():
    check_only = "--check" in sys.argv
    with open(TARGET, "r", encoding="utf-8") as fh:
        text = fh.read()

    unmapped = []
    replaced = 0

    # The source reads `m_dark ? DARK : LIGHT`, so the regex yields the dark
    # triple first. The table above is keyed by (light, dark).
    by_source_order = {}
    for (light, dark), role in MAP.items():
        if role is None:
            continue
        by_source_order[(dark, light)] = role

    def repl(match):
        nonlocal replaced
        dr, dg, db = (int(match.group(i)) for i in (1, 2, 3))
        lr, lg, lb = (int(match.group(i)) for i in (4, 5, 6))
        role = by_source_order.get(((dr, dg, db), (lr, lg, lb)))
        if role is None:
            unmapped.append(((lr, lg, lb), (dr, dg, db)))
            return match.group(0)
        replaced += 1
        return "th().%s" % role

    new_text = PAIR_RE.sub(repl, text)

    if check_only:
        by_source = {}
        for (light, dark), role in MAP.items():
            if role is None:
                continue
            by_source[(dark, light)] = role
        left = []
        for match in PAIR_RE.finditer(text):
            dr, dg, db = (int(match.group(i)) for i in (1, 2, 3))
            lr, lg, lb = (int(match.group(i)) for i in (4, 5, 6))
            if ((dr, dg, db), (lr, lg, lb)) not in by_source:
                left.append(((lr, lg, lb), (dr, dg, db)))
        print("inline colour pairs remaining: %d" % len(left))
        for pair in sorted(set(left)):
            print("  unmapped: light=%s dark=%s" % (pair[0], pair[1]))
        return 1 if left else 0

    if unmapped:
        print("ERROR: unmapped colour pairs, nothing written:", file=sys.stderr)
        for pair in sorted(set(unmapped)):
            print("  light=%s dark=%s" % (pair[0], pair[1]), file=sys.stderr)
        return 1

    with open(TARGET, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new_text)
    print("editor_win.h: replaced %d inline colour pairs" % replaced)

    # Append the new roles to the theme's X-macro list.
    with open(THEME, "r", encoding="utf-8") as fh:
        theme = fh.read()

    existing = set(re.findall(r"X\(\s*(\w+)\s*,", theme))
    added = []
    for role, (light, dark) in NEW_ROLES.items():
        if role in existing:
            continue
        added.append("    X(%s, %d, %d, %d,  %d, %d, %d)"
                     % (role, light[0], light[1], light[2], dark[0], dark[1], dark[2]))
    if added:
        marker = "    X(kCommitFg, 255, 255, 255, 255, 255, 255)"
        theme = theme.replace(marker, marker + "\n" + "\n".join(added))
        with open(THEME, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(theme)
        print("editor_theme.h: added %d roles" % len(added))
    return 0


if __name__ == "__main__":
    sys.exit(main())
