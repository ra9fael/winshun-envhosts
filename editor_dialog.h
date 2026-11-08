// editor_dialog.h - the v0.7.0 themed modal dialog.
//
// Why this exists
// ---------------
// Until v0.7.0 every confirmation used MessageBoxW, which renders a bare
// system-styled text window that clashes with the flat self-drawn editor.
// This module draws the dialog with the SAME palette the editor uses
// (editor_theme.h), following the custom-dialog design draft: an icon
// bubble, a title with a dim subtitle, a wrapped body, an optional
// monospace detail block and right-aligned flat buttons.
//
// Compatibility with the GUI regression driver
// --------------------------------------------
// The window is created on the SYSTEM dialog class "#32770" and every
// button is a real HWND BUTTON (owner-drawn). Tests that look for class
// "#32770", read the body via GetDlgItem(dlg, 0xFFFF) and click buttons
// with GetDlgItem / BM_CLICK keep working unchanged.
//
// Usage
// -----
//     envh::dlg::Spec s;
//     s.title = L"删除备份";
//     s.body  = wideFromUtf8("删除备份文件 xxx？此操作不可撤销。");
//     s.icon  = envh::dlg::kDanger;
//     s.buttons = {{L"取消", IDCANCEL, envh::dlg::kPlain},
//                  {L"删除", IDYES, envh::dlg::kDangerBtn}};
//     s.defButton = 0;
//     const int id = envh::dlg::show(m_hwnd, s, th(), m_dark);
//
// The call runs a modal message loop, disables the owner and returns the
// pressed button's id (Enter presses the default button, Esc presses the
// button with the IDCANCEL id).

#pragma once

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <vector>

#include "editor_theme.h"

namespace envh::dlg {

enum Icon { kInfo = 0, kWarn = 1, kDanger = 2 };
enum BtnKind { kPlain = 0, kPrimary = 1, kDangerBtn = 2 };

struct Button {
    std::wstring label;
    int id;
    int kind;
};

struct Spec {
    std::wstring title;    // title row text (also used as the fallback)
    std::wstring subtitle; // dim line under the title
    std::wstring body;     // wrapped body text
    std::wstring detail;   // optional monospace block
    Icon icon = kInfo;
    std::vector<Button> buttons;
    int defButton = 0; // buttons[] index the Enter key presses
};

struct State {
    Spec spec;
    envh::theme::Th th;
    bool dark = false;
    bool done = false;
    int result = 0;
    int hoverId = 0;
    bool tracking = false;
    RECT detailRect = {};
    HFONT ui = nullptr, small = nullptr, mono = nullptr;
    HBRUSH background = nullptr;
    ~State()
    {
        if (ui) DeleteObject(ui);
        if (small) DeleteObject(small);
        if (mono) DeleteObject(mono);
        if (background) DeleteObject(background);
    }
};

State* stateOf(HWND h)
{
    return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

void drawButtonFace(HDC dc, const RECT& r, const std::wstring& label, int kind,
                    bool def, bool hovered, bool focused, const envh::theme::Th& th,
                    HFONT font)
{
    // Owner-drawn buttons must also paint the corners outside the rounded
    // face; the native BUTTON background otherwise leaves white squares.
    HBRUSH background = CreateSolidBrush(th.kBg);
    FillRect(dc, &r, background);
    DeleteObject(background);
    COLORREF bg, br, fg;
    if (kind == kDangerBtn) {
        bg = th.kDanger;
        br = th.kDanger;
        fg = th.kCommitFg;
    } else if (kind == kPrimary) {
        bg = th.kAccent;
        br = th.kAccentDeep;
        fg = th.kCommitFg;
    } else {
        bg = hovered ? th.kBtnHoverBg : th.kChipBg;
        br = hovered ? th.kBtnEdge : th.kChipEdge;
        fg = th.kText;
    }
    // v2 design: rounded buttons (6px radius) instead of hard squares.
    HBRUSH b = CreateSolidBrush(bg);
    HGDIOBJ ob = SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, br);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 12, 12);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
    DeleteObject(b);
    // A thicker ring marks the default (Enter) button.
    if (def && kind == kPlain) {
        HPEN dp = CreatePen(PS_SOLID, 1, th.kAccentText);
        HGDIOBJ odp = SelectObject(dc, dp);
        HGDIOBJ odb = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, 10, 10);
        SelectObject(dc, odb);
        SelectObject(dc, odp);
        DeleteObject(dp);
    }
    if (focused) {
        RECT focus = r;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(dc, &focus);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HGDIOBJ of = SelectObject(dc, font);
    RECT t = r;
    DrawTextW(dc, label.c_str(), -1, &t,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

// Measures and lays out the dialog content. Returns the window height for
// the given width; fills outBody / outDetail for the static control and
// the paint pass.
int contentHeight(HWND hdlg, State& st, int w, RECT& bodyRc, RECT& detailRc,
                  std::vector<RECT>& btnRects, bool /*forPaint*/)
{
    const int pad = 24;
    const int headH = 84; // icon tile + title + subtitle (v2: no header band)
    // Measure before creating children using the SAME font and wrapping as
    // the body control. Its existence must not change the window height.
    RECT meas = {0, 0, w - 2 * pad, 0};
    {
        HDC dc = GetDC(hdlg);
        HGDIOBJ of = SelectObject(dc, st.ui);
        RECT br = meas;
        DrawTextW(dc, st.spec.body.c_str(), -1, &br,
                  DT_LEFT | DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT);
        SelectObject(dc, of);
        ReleaseDC(hdlg, dc);
        bodyRc = {pad, headH, pad + (w - 2 * pad), headH + (br.bottom - br.top)};
    }
    int y = bodyRc.bottom;
    if (!st.spec.detail.empty()) {
        RECT dr = {0, 0, w - 2 * pad - 24, 0};
        HDC dc = GetDC(hdlg);
        HGDIOBJ of = SelectObject(dc, st.mono);
        DrawTextW(dc, st.spec.detail.c_str(), -1, &dr,
                  DT_LEFT | DT_NOPREFIX | DT_WORDBREAK | DT_CALCRECT);
        SelectObject(dc, of);
        ReleaseDC(hdlg, dc);
        detailRc = {pad, y + 14, w - pad, y + 14 + (dr.bottom - dr.top) + 20};
        y = detailRc.bottom;
    } else {
        SetRectEmpty(&detailRc);
    }
    const int btnH = 30;
    const int btnTop = y + 24 + btnH + 18;
    // Right-aligned button row.
    btnRects.clear();
    int x = w - pad;
    HDC buttonDc = GetDC(hdlg);
    HGDIOBJ oldFont = SelectObject(buttonDc, st.ui);
    for (std::size_t i = st.spec.buttons.size(); i-- > 0;) {
        SIZE label {};
        GetTextExtentPoint32W(buttonDc, st.spec.buttons[i].label.c_str(),
                             static_cast<int>(st.spec.buttons[i].label.size()), &label);
        const int bw = std::max(st.spec.buttons[i].kind == kPlain ? 84 : 108,
                                static_cast<int>(label.cx) + 24);
        x -= bw;
        btnRects.push_back({x, btnTop - btnH - 18, x + bw, btnTop - 18});
        x -= 10;
    }
    SelectObject(buttonDc, oldFont);
    ReleaseDC(hdlg, buttonDc);
    return btnTop;
}

LRESULT CALLBACK dlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    State* st = stateOf(h);
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        State* s = static_cast<State*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        return TRUE;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_CLOSE:
        SendMessageW(h, WM_COMMAND, IDCANCEL, 0);
        return 0;
    case WM_COMMAND: {
        if (!st)
            break;
        const int id = LOWORD(wp);
        if (HIWORD(wp) == BN_CLICKED) {
            // Map synthetic dialog commands (Enter -> IDOK, Esc -> IDCANCEL)
            // onto real buttons: IDOK presses the default button, IDCANCEL
            // presses the button carrying the IDCANCEL id (or the last
            // plain one when the spec has no cancel).
            if (id == IDOK || id == IDCANCEL) {
                int target = -1;
                for (std::size_t i = 0; i < st->spec.buttons.size(); ++i)
                    if (st->spec.buttons[i].id == id)
                        target = static_cast<int>(i);
                if (target < 0 && id == IDOK)
                    target = st->spec.defButton;
                if (target < 0)
                    for (std::size_t i = st->spec.buttons.size(); i-- > 0;)
                        if (st->spec.buttons[i].kind == kPlain)
                            target = static_cast<int>(i);
                if (target < 0)
                    target = st->spec.defButton;
                st->result = st->spec.buttons[static_cast<std::size_t>(target)].id;
                st->done = true;
            } else {
                st->result = id;
                st->done = true;
            }
            // The click may arrive while the modal loop is still blocked in
            // GetMessage (a cross-thread BM_CLICK); wake it up.
            PostMessageW(h, WM_NULL, 0, 0);
            return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!st || dis->CtlType != ODT_BUTTON)
            break;
        const int id = static_cast<int>(dis->CtlID);
        std::wstring label;
        int kind = kPlain;
        bool def = false;
        for (std::size_t i = 0; i < st->spec.buttons.size(); ++i) {
            if (st->spec.buttons[i].id == id) {
                label = st->spec.buttons[i].label;
                kind = st->spec.buttons[i].kind;
                def = static_cast<int>(i) == st->spec.defButton;
            }
        }
        drawButtonFace(dis->hDC, dis->rcItem, label, kind, def,
                       st->hoverId == id, (dis->itemState & ODS_FOCUS) != 0,
                       st->th, st->ui);
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        if (!st)
            break;
        const POINT p {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        HWND hit = ChildWindowFromPoint(h, p);
        DWORD cid = hit ? GetWindowLongPtrW(hit, GWLP_ID) : 0;
        const int id = static_cast<int>(cid);
        if (id != st->hoverId) {
            st->hoverId = id;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (!st->tracking) {
            TRACKMOUSEEVENT tm {sizeof(tm), TME_LEAVE, h, 0};
            TrackMouseEvent(&tm);
            st->tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (st) {
            st->tracking = false;
            if (st->hoverId) {
                st->hoverId = 0;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_PAINT: {
        if (!st)
            break;
        const envh::theme::Th& th = st->th;
        RECT cr {};
        GetClientRect(h, &cr);
        const int w = cr.right;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        HBRUSH bgb = CreateSolidBrush(th.kBg);
        FillRect(mem, &cr, bgb);
        DeleteObject(bgb);
        // v2 design: no header band - a clean sheet with an icon tile.
        // Icon tile: rounded square, soft tint, glyph centred.
        COLORREF ib, ibr, ifg;
        if (st->spec.icon == kDanger) {
            ib = th.kDialogDangerBg, ibr = th.kDanger, ifg = th.kDanger;
        } else if (st->spec.icon == kWarn) {
            ib = th.kDialogWarnBg, ibr = th.kWarn, ifg = th.kWarn;
        } else {
            ib = th.kDialogInfoBg, ibr = th.kScopeUserBr, ifg = th.kAccentText;
        }
        HBRUSH ibb = CreateSolidBrush(ib);
        HGDIOBJ oibb = SelectObject(mem, ibb);
        HPEN ibp = CreatePen(PS_SOLID, 1, ibr);
        HGDIOBJ oibp = SelectObject(mem, ibp);
        RoundRect(mem, 24, 24, 64, 64, 12, 12);
        SelectObject(mem, oibp);
        SelectObject(mem, oibb);
        DeleteObject(ibp);
        DeleteObject(ibb);
        SetBkMode(mem, TRANSPARENT);
        const wchar_t* glyph = st->spec.icon == kDanger ? L"!"
            : st->spec.icon == kWarn ? L"⚠"
                                     : L"i";
        SetTextColor(mem, ifg);
        HGDIOBJ og = SelectObject(mem, st->ui);
        RECT gr {24, 24, 64, 64};
        DrawTextW(mem, glyph, -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, og);
        // Title + subtitle beside the tile.
        RECT tr {80, 24, w - 24, 48};
        SetTextColor(mem, th.kText);
        SelectObject(mem, st->ui);
        DrawTextW(mem, st->spec.title.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!st->spec.subtitle.empty()) {
            RECT sr {80, 48, w - 24, 70};
            SelectObject(mem, st->small);
            SetTextColor(mem, th.kTextDim);
            DrawTextW(mem, st->spec.subtitle.c_str(), -1, &sr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        // Detail block (monospace, panel-alt background, accent bar on the
        // left edge, per the v2 draft).
        SelectObject(mem, st->ui);
        if (st->detailRect.right > st->detailRect.left) {
            RECT dr = st->detailRect;
            HBRUSH db = CreateSolidBrush(th.kPanelAlt);
            FillRect(mem, &dr, db);
            DeleteObject(db);
            HPEN dp = CreatePen(PS_SOLID, 1, th.kBorderSoft);
            HGDIOBJ odp = SelectObject(mem, dp);
            HGDIOBJ odb = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, dr.left, dr.top, dr.right, dr.bottom);
            SelectObject(mem, odb);
            SelectObject(mem, odp);
            DeleteObject(dp);
            HBRUSH ab = CreateSolidBrush(th.kAccent);
            RECT bar {dr.left, dr.top, dr.left + 3, dr.bottom};
            FillRect(mem, &bar, ab);
            DeleteObject(ab);
            RECT tx = dr;
            tx.left += 12;
            tx.right -= 12;
            tx.top += 8;
            SelectObject(mem, st->mono);
            SetTextColor(mem, th.kTextDim2);
            DrawTextW(mem, st->spec.detail.c_str(), -1, &tx,
                      DT_LEFT | DT_NOPREFIX | DT_WORDBREAK);
            SelectObject(mem, st->ui);
        }
        // Hairline divider above the button row.
        const int sep = cr.bottom - 60;
        if (sep > 0) {
            HPEN sp = CreatePen(PS_SOLID, 1, th.kBorderSoft);
            HGDIOBJ osp = SelectObject(mem, sp);
            MoveToEx(mem, 24, sep, nullptr);
            LineTo(mem, w - 24, sep);
            SelectObject(mem, osp);
            DeleteObject(sp);
        }
        BitBlt(dc, 0, 0, w, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC: {
        // The body static (0xFFFF) must paint on the dialog background, not
        // the default gray STATIC brush - that gray block was the main
        // "ugly" complaint about v1.
        if (reinterpret_cast<HWND>(lp) == GetDlgItem(h, 0xFFFF)) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, st ? st->th.kText : GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(st ? st->background : GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Show the modal dialog; returns the id of the pressed button.
inline int show(HWND owner, const Spec& spec, const envh::theme::Th& th, bool dark)
{
    // Register a PROCESS-LOCAL class named "#32770". It shadows the system's
    // global dialog class for windows created by this process, so our own
    // wndproc runs (a bare CreateWindowEx on the global class would be
    // handled by DefDlgProc and our dlgProc would never see a message).
    // The class name keeps test drivers that look for standard dialogs
    // (GetDlgItem / BM_CLICK / ctrl 0xFFFF) working unchanged.
    static const bool clsRegistered = [] {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_SAVEBITS;
        wc.lpfnWndProc = dlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"#32770";
        RegisterClassExW(&wc); // ERROR_CLASS_EXISTS is fine - already ours
        return true;
    }();
    (void)clsRegistered;

    State* st = new State();
    st->spec = spec;
    st->th = th;
    st->dark = dark;
    st->background = CreateSolidBrush(th.kBg);
    st->result = spec.buttons.empty() ? IDOK : spec.buttons[spec.defButton].id;
    st->ui = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    st->small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    st->mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    RECT orc {};
    GetWindowRect(owner, &orc);
    const int w = std::min(620, std::max(420, static_cast<int>((orc.right - orc.left) - 40)));

    // Pre-measure with a temporary hidden window so the height is right.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"#32770", L"",
                             WS_POPUP | WS_CLIPCHILDREN, 0, 0, w, 200,
                             owner, nullptr, GetModuleHandleW(nullptr), st);
    if (!h) {
        delete st;
        return IDCANCEL;
    }
    RECT bodyRc {}, detailRc {};
    std::vector<RECT> btns;
    const int height = contentHeight(h, *st, w, bodyRc, detailRc, btns, false);
    st->detailRect = detailRc;
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - height) / 3;
    MONITORINFO monitor {};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        x = std::max(static_cast<int>(monitor.rcWork.left),
                     std::min(x, static_cast<int>(monitor.rcWork.right) - w));
        y = std::max(static_cast<int>(monitor.rcWork.top),
                     std::min(y, static_cast<int>(monitor.rcWork.bottom) - height));
    }
    SetWindowPos(h, HWND_TOP, x, y, w, height, SWP_NOACTIVATE);

    // Body as a REAL static control (id 0xFFFF) so test drivers can read it.
    HWND bodyCtl = CreateWindowExW(0, L"STATIC", st->spec.body.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL | SS_NOPREFIX,
                                   bodyRc.left, bodyRc.top,
                                   bodyRc.right - bodyRc.left,
                                   bodyRc.bottom - bodyRc.top,
                                   h, reinterpret_cast<HMENU>(0xFFFF),
                                   GetModuleHandleW(nullptr), nullptr);
    if (bodyCtl) {
        SendMessageW(bodyCtl, WM_SETFONT, reinterpret_cast<WPARAM>(st->ui), TRUE);
    }
    for (const auto& b : spec.buttons) {
        CreateWindowExW(0, L"BUTTON", b.label.c_str(),
                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                        0, 0, 10, 10, h, reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(b.id)),
                        GetModuleHandleW(nullptr), nullptr);
    }
    // Position every child using the same measurement as the window itself.
    if (bodyCtl)
        MoveWindow(bodyCtl, bodyRc.left, bodyRc.top,
                   bodyRc.right - bodyRc.left, bodyRc.bottom - bodyRc.top, TRUE);
    int bi = 0;
    for (const auto& b : spec.buttons) {
        const RECT& r = btns[btns.size() - 1 - bi];
        HWND bc = GetDlgItem(h, b.id);
        if (bc)
            MoveWindow(bc, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        ++bi;
    }

    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOWNORMAL);
    HWND defBtn = GetDlgItem(h, spec.buttons[spec.defButton].id);
    if (defBtn)
        SetFocus(defBtn);

    MSG m {};
    while (!st->done) {
        const BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0)
                PostQuitMessage(static_cast<int>(m.wParam));
            break;
        }
        // IsDialogMessage handles Tab (focus), Enter (default button) and
        // Esc (IDCANCEL command) with native dialog semantics.
        if (IsDialogMessageW(h, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    const int result = st->result;
    DestroyWindow(h);
    delete st;
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return result;
}

// Convenience wrapper for the common two-button question. Returns IDYES/IDNO.
inline int ask(HWND owner, const envh::theme::Th& th, bool dark,
               const std::wstring& title, const std::wstring& body,
               Icon icon, bool defYes, const wchar_t* yesLabel = nullptr,
               const wchar_t* noLabel = nullptr)
{
    Spec s;
    s.title = title;
    s.body = body;
    s.icon = icon;
    s.buttons = {
        {noLabel ? noLabel : L"取消", IDCANCEL, kPlain},
        {yesLabel ? yesLabel : L"确定", IDYES, icon == kDanger ? kDangerBtn : kPrimary},
    };
    s.defButton = defYes ? 1 : 0;
    return show(owner, s, th, dark);
}

} // namespace envh::dlg
