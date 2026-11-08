// editor_theme.h - single source of truth for the editor's colours and metrics.
//
// Why this file exists
// --------------------
// The editor paints with plain GDI (no GDI+). Until v0.7.0 every colour was
// an inline RGB(...) literal at the call site - 113 of them, 81 of them in
// `m_dark ? RGB(a) : RGB(b)` pairs. The SVG design drafts in docs/design were
// therefore hand-written and drifted from the code: the same "user scope
// chip" was #e8f1ff in the draft and RGB(228,238,252) in the painter.
//
// Now every colour and every shared metric lives here, the painter reads it
// from here, and tools/gen_design_svg.py parses THIS FILE to emit the SVG
// drafts. Draft and product share one definition, so they cannot drift.
//
// How the colours are declared
// ----------------------------
// One X-macro list holds each role together with its light and dark value:
//
//     X(kScopeUserBg, 230,241,251,  44, 60, 80)
//
// The list is expanded twice: once into the Th struct (light) and once into
// the runtime fill (dark). A Th instance therefore carries ONE concrete value
// per role, and `th().kText` dispatches at runtime - which is what the
// painter needs, and what the SVG generator reads.
//
// The generator understands exactly this syntax. Keep new entries in it, or
// teach the generator the new form.
//
//     static constexpr COLORREF kName = ENVH_RGB(r, g, b);   // one-off
//     X(kName, r, g, b, dr, dg, db);                          // themed

#pragma once

#include <windows.h>

// Pack an r/g/b triple into a COLORREF (0x00BBGGRR).
#define ENVH_RGB(r, g, b) \
    (COLORREF)((((DWORD)(b) & 0xFF) << 16) | (((DWORD)(g) & 0xFF) << 8) | ((DWORD)(r) & 0xFF))

// role, light rgb, dark rgb
#define ENVH_THEME_FIELDS(X)                                                                \
    /* surfaces */                                                                          \
    X(kBg, 255, 255, 255, 24, 26, 30)                                                       \
    X(kPanel, 255, 255, 255, 30, 33, 38)                                                    \
    X(kPanelAlt, 251, 251, 252, 38, 41, 46)                                                 \
    X(kHeadBg, 241, 239, 232, 40, 44, 50)                                                   \
    X(kFieldBg, 255, 255, 255, 24, 26, 30)                                                  \
    X(kBorder, 200, 206, 214, 70, 76, 84)                                                   \
    X(kBorderSoft, 226, 229, 234, 60, 64, 70)                                               \
    /* text */                                                                              \
    X(kText, 31, 35, 40, 232, 234, 237)                                                     \
    X(kTextDim, 110, 110, 110, 120, 128, 136)                                               \
    X(kTextMuted, 156, 163, 175, 144, 152, 160)                                             \
    /* accent */                                                                            \
    X(kAccent, 88, 166, 255, 120, 180, 250)                                                 \
    X(kAccentDeep, 55, 138, 221, 120, 180, 250)                                             \
    X(kSelBg, 210, 228, 250, 38, 42, 48)                                                    \
    X(kSelBar, 55, 138, 221, 120, 180, 250)                                                 \
    X(kFocusRing, 230, 241, 251, 44, 60, 80)                                                \
    /* status */                                                                            \
    X(kModified, 180, 130, 40, 235, 190, 100)                                               \
    X(kAdded, 30, 120, 50, 120, 200, 130)                                                   \
    X(kDeleted, 170, 50, 50, 230, 130, 130)                                                 \
    X(kWarn, 150, 104, 26, 170, 130, 40)                                                    \
    X(kDanger, 196, 62, 62, 232, 104, 104)                                                 \
    /* marked row backgrounds */                                                             \
    X(kDirtyBg, 255, 240, 240, 66, 38, 42)                                                  \
    X(kOverBg, 255, 226, 226, 92, 40, 44)                                                   \
    X(kNewBg, 238, 247, 234, 38, 62, 38)                                                    \
    X(kStripe, 247, 249, 252, 30, 33, 38)                                                   \
    /* scope chips */                                                                        \
    X(kScopeUserBg, 230, 241, 251, 44, 60, 80)                                              \
    X(kScopeUserFg, 24, 95, 165, 180, 215, 255)                                             \
    X(kScopeUserBr, 158, 197, 255, 76, 108, 124)                                            \
    X(kScopeSysBg, 243, 232, 255, 58, 44, 68)                                               \
    X(kScopeSysFg, 107, 33, 168, 212, 174, 245)                                             \
    X(kScopeSysBr, 201, 166, 245, 92, 68, 116)                                              \
    /* diff verdicts */                                                                      \
    X(kDiffSame, 30, 120, 50, 120, 200, 130)                                                \
    X(kDiffValue, 160, 100, 20, 230, 170, 90)                                               \
    X(kDiffOnly, 30, 120, 50, 120, 200, 130)                                                \
    X(kDiffExtra, 170, 50, 50, 230, 120, 120)                                               \
    /* commit button */                                                                      \
    X(kCommitBg, 22, 154, 58, 22, 154, 58)                                                  \
    X(kCommitBr, 21, 128, 61, 21, 128, 61)                                                  \
    X(kCommitFg, 255, 255, 255, 255, 255, 255)                                              \
    X(kSubHeadBg, 238, 241, 246,  42, 46, 53)                                               \
    X(kSectionBg, 238, 241, 246,  36, 40, 46)                                               \
    X(kDiffBg, 250, 251, 253,  30, 33, 39)                                                  \
    X(kStatusBg, 236, 239, 244,  38, 41, 46)                                                 \
    X(kStatusRow1Bg, 232, 236, 242,  36, 40, 46)                                            \
    X(kStatusRow2Bg, 241, 243, 247,  30, 33, 38)                                            \
    X(kPreviewSelBg, 228, 238, 252,  38, 42, 48)                                           \
    X(kMultiSelBg, 228, 238, 252,  48, 60, 76)                                              \
    X(kFocusBar, 60, 130, 215,  120, 180, 250)                                              \
    X(kTabBg, 228, 232, 238,  48, 52, 58)                                                   \
    X(kStatusSelBg, 230, 241, 251,  36, 48, 62)                                             \
    X(kChipInfoBg, 230, 241, 251,  32, 48, 68)                                              \
    X(kPartSelBg, 230, 241, 251,  38, 48, 62)                                               \
    X(kPartAddBg, 252, 235, 235,  52, 34, 36)                                               \
    X(kChipDangerBg, 252, 235, 235,  72, 36, 38)                                            \
    X(kTextDim2, 110, 116, 126,  150, 156, 166)                                             \
    X(kMutedText, 130, 130, 130,  130, 136, 144)                                            \
    X(kAddedText, 24, 95, 165,  130, 180, 240)                                               \
    X(kChipInfoFg, 24, 95, 165,  150, 195, 250)                                              \
    X(kAccentText, 24, 95, 165,  180, 215, 255)                                              \
    X(kDeletedText, 163, 45, 45,  236, 150, 150)                                             \
    X(kChipDangerFg, 163, 45, 45,  240, 150, 150)                                            \
    X(kOverText, 170, 46, 46,  244, 150, 150)                                               \
    X(kRemovedText, 170, 60, 60,  220, 120, 120)                                             \
    X(kDiffRed, 176, 44, 44,  240, 120, 120)                                                 \
    X(kDiffBlue, 28, 92, 168,  120, 180, 250)                                                \
    X(kChipTextStrong, 12, 68, 124,  180, 215, 255)                                          \
    X(kWarnText, 150, 96, 12,  230, 186, 100)                                                \
    X(kElevText, 202, 138, 30,  170, 130, 40)                                               \
    X(kChipBorder, 68, 68, 65,  220, 224, 230)                                               \
    X(kChipElevBg, 190, 196, 204,  90, 96, 104)                                             \
    X(kChipOffBg, 236, 238, 241,  40, 43, 48)                                                \
    X(kChipEdge, 220, 222, 226,  55, 58, 64)                                                \
    X(kChipBg, 255, 255, 255,  48, 52, 58)                                                  \
    X(kGridPen, 160, 168, 180,  96, 104, 116)                                               \
    X(kPillEdge, 133, 183, 235,  80, 118, 168)                                               \
    X(kBtnBg, 242, 242, 242,  42, 45, 50)                                                    \
    X(kBtnActiveBg, 215, 228, 244,  60, 66, 76)                                              \
    X(kBtnHoverBg, 225, 232, 241,  48, 52, 58)                                               \
    X(kBtnEdge, 176, 188, 202,  90, 98, 110)                                                 \
    X(kBtnEdgeDim, 220, 222, 226,  60, 64, 70)                                               \
    X(kDangerSoft, 226, 75, 74,  226, 100, 100)                                                   \
    X(kWarnBg, 250, 238, 218,  44, 48, 54)                                                   \
    X(kSuccessBg, 234, 243, 222,  36, 52, 40)                                                 \
    X(kSuccessText, 59, 109, 22,  150, 210, 130)                                              \
    X(kErrorBand, 140, 52, 52,  140, 52, 52)                                                   \
    X(kOverBand, 150, 58, 48,  150, 58, 48)                                                     \
    /* v0.7.2: foregrounds for the always-on notice bands. The bands are dark  \
       and saturated, so the old shared kWarnText (150,96,12 on 140,52,52)   \
       was unreadable. These pairs clear WCAG AA (>= 4.5:1) in both themes.  \
         kErrorBand  (140,52,52) + white  -> 7.9:1                          \
         kOverBand   (150,58,48) + white  -> 7.1:1                          \
         kWarn light (150,104,26) + white -> 4.9:1                          \
         kWarn dark  (170,130,40) + ink   -> 5.1:1                          */                 \
    X(kBandErrFg, 255, 255, 255, 255, 255, 255)                                                \
    X(kBandOverFg, 255, 255, 255, 255, 255, 255)                                               \
    X(kBandWarnFg, 255, 255, 255, 24, 18, 8)                                                   \
    /* v0.7.0 themed dialog icon bubbles */                                                     \
    X(kDialogDangerBg, 254, 226, 226,  70, 32, 36)                                              \
    X(kDialogWarnBg, 254, 243, 199,  66, 52, 24)                                                \
    X(kDialogInfoBg, 230, 241, 251,  32, 48, 68)

namespace envh::theme {

// Layout metrics shared by the painter and the design drafts.
struct Metrics {
    static constexpr int kRowH = 24;            // main table row height
    static constexpr int kHeaderH = 26;         // main table column header height
    static constexpr int kBandH = 22;           // one always-on notice band
    static constexpr int kTabH = 28;            // top tab strip height
    static constexpr int kBackupScopeBarH = 26; // backup page scope filter row
    static constexpr int kBackupHeadH = 20;     // backup preview column header
    static constexpr int kBackupListRowH = 44;  // backup list entry (two lines)
    static constexpr int kPartNumW = 46;        // "#" column
    static constexpr int kPartCharsW = 76;      // per-row character count
    static constexpr int kPartActW = 84;        // state tag / x
    static constexpr int kPillRadius = 12;      // RoundRect ellipse diameter
};

// The runtime palette. One instance per theme; the painter keeps a pointer to
// whichever one is active, so `th().kText` resolves per theme at runtime.
struct Th {
#define ENVH_DECL_FIELD(name, r, g, b, dr, dg, db) COLORREF name = ENVH_RGB(r, g, b);
    ENVH_THEME_FIELDS(ENVH_DECL_FIELD)
#undef ENVH_DECL_FIELD

    // Fill in the dark values of every role.
    static Th dark()
    {
        Th t;
#define ENVH_SET_DARK(name, r, g, b, dr, dg, db) t.name = ENVH_RGB(dr, dg, db);
        ENVH_THEME_FIELDS(ENVH_SET_DARK)
#undef ENVH_SET_DARK
        return t;
    }

    // Fill in the light values of every role (the defaults already are).
    static Th light()
    {
        return Th {};
    }
};

} // namespace envh::theme
