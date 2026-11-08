// editor_win.h - the interactive editor window for ra9fael.envhosts (v0.2).
//
// One GDI-painted window per kind ("env" | "hosts"), each on its own GUI thread,
// modelled on ra9fael.binarylab/editor.h (mutex + condition_variable + dedicated
// thread, no stdout writes from the GUI thread). The protocol thread only calls
// the free functions openEditor / isEditorOpen / onExternalChange / closeAllEditors.
//
// The window owns no Win32 registry/file I/O directly: it reads and writes
// through native.h (EnvStore / HostsStore / BackupStore) while holding
// native::g_writeLock, and edits through the pure editor_model.h. Everything
// that can be unit-tested lives in editor_model.h; this file is the view + glue.
//
// Keyboard map (design doc section 7, revised v0.7.1): Tab/Ctrl+Tab, Ctrl+F,
// arrows, Enter/F2, Space (hosts toggle), Ctrl+N (add item), Del/Backspace
// (stage-delete items), Ctrl+U (delete whole row), Ctrl+J/K (item caret),
// Alt+J/K/Up/Down (move item), Ctrl+R (PATH list/raw), Ctrl+C, Ctrl+Z/Y,
// Ctrl+S, F5, Esc. Vim-style, no modifiers: j/k/gg/G with a digit count
// prefix (3j, 5G), x/dd stage-delete, u undo, / filter, l/h open/return on
// the main table; bare j/k move the item-list caret as well.
//
// Code and comments are English.
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "winshun.h"
#include "logic.h"
#include "native.h"
#include "editor_model.h"
#include "editor_common.h"
#include "draft_storage.h"
#include "editor_theme.h"
#include "editor_dialog.h"

namespace editorwin {

constexpr UINT WM_APP_FOCUS = WM_APP + 1; // apply a new OpenArgs (keep edits)
constexpr UINT WM_APP_EXTCHANGE = WM_APP + 2; // external target change
constexpr UINT WM_APP_APPEARANCE = WM_APP + 3; // host language / theme changed
// Read-only test hooks used by tests/gui_preview.py. WM_APP_KEY carries an
// explicit modifier mask in lParam (bit 0 Ctrl, bit 1 Shift) because injected
// WM_KEYDOWN messages cannot set the real asynchronous key state.
constexpr UINT WM_APP_KEY = WM_APP + 4;
// Returns a read-only snapshot: bits 0-15 selected visible rows, 16-31 rows
// marked deleted, 32-47 disabled hosts rows, 48-55 UI flags
// (bit0 diff panel open, bit1 external change pending, bit2 over-length rows,
// bits 3-7 of the flags byte the staged item-delete count), 56-63 the caret
// row (visible index).
// No pointers cross the process boundary, so SendMessageW can be used directly
// from the GUI test driver.
constexpr UINT WM_APP_GETSTATE = WM_APP + 5;
// v0.7.2: geometry probe. wParam selects the rect (0 diff panel, 1 diff
// footer, 2 diff reload button, 3 item hint, 4 commit button, 5 apply button,
// 6 delimiter box, 7 notice band) and lParam the notice-band index for the
// two colour queries (8 = foreground, 9 = background). A rect comes back as
// left | top<<16 | right<<32 | bottom<<48.
constexpr UINT WM_APP_GETLAYOUT = WM_APP + 6;

struct AppearanceArgs {
    envh::i18n::Lang lang = envh::i18n::Lang::ZhCN;
    bool dark = false;
};

// Per-kind thread context: the protocol thread creates one of these, launches the
// EditorWindow on a dedicated GUI thread, and uses the HWND/flags to focus or close
// it. The window reports its lifecycle back through m_ctx.
struct ThreadCtx {
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    HWND hwnd = nullptr;
    bool running = false;
    bool visible = false;
    std::string error;
};

// ---- the window --------------------------------------------------------------
class EditorWindow {
public:
    explicit EditorWindow(OpenArgs args) : m_args(std::move(args)), m_kind(m_args.kind)
    {
        m_dark = m_args.dark;
        applyTheme();
        m_filterText = m_args.filter;
    }

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Builds the model and runs the message loop. Returns when the window closes.
    void setContext(ThreadCtx* c) { m_ctx = c; }
    void run()
    {
        loadModel();
        ensureDpi();
        registerClass();
        computeLayout();

        // CreateWindowExW takes the OUTER window rect. Convert the desired client
        // size so the layout coordinates (which are client-relative) actually fit.
        // WS_THICKFRAME stays on so the table can be enlarged; WS_VSCROLL gives
        // the virtual list a real scrollbar (keyboard, mouse and accessibility).
        const DWORD style = windowStyle();
        RECT wr {0, 0, m_clientW, m_clientH};
        AdjustWindowRectEx(&wr, style, FALSE, 0);
        m_hwnd = CreateWindowExW(0, kClass, title().c_str(), style,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 wr.right - wr.left, wr.bottom - wr.top,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_hwnd) {
            winshun::debug("ra9fael.envhosts: CreateWindowExW failed (gle " + std::to_string(GetLastError()) + ")");
            if (m_ctx) {
                std::lock_guard<std::mutex> lk(m_ctx->m);
                m_ctx->error = "CreateWindowExW failed (gle " + std::to_string(GetLastError()) + ")";
                m_ctx->cv.notify_all();
            }
            return;
        }
        if (m_ctx) {
            std::lock_guard<std::mutex> lk(m_ctx->m);
            m_ctx->hwnd = m_hwnd;
            m_ctx->visible = true;
            m_ctx->cv.notify_all();
        }
        m_editFont = makeFont(13, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        createControls();
        applyLayout();
        selectRow(0);
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        UpdateWindow(m_hwnd);
        showAndActivate();
        offerDraft();
        SetTimer(m_hwnd, kTimerDraft, 30000, nullptr);
        // Poll the real target every 2s. A dedicated waiting thread with
        // RegNotifyChangeKeyValue would keep the process permanently resident
        // for a few dozen values plus one file; polling is cheaper here and
        // keeps query() side-effect free.
        SetTimer(m_hwnd, kTimerWatch, 2000, nullptr);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            try {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            } catch (const std::exception& e) {
                winshun::debug(std::string("ra9fael.envhosts: loop: ") + e.what());
            } catch (...) {
                winshun::debug("ra9fael.envhosts: loop: unknown");
            }
        }
        persistDraftIfDirty();
    }

    // Bring the window up front once per user action, never permanently: the
    // launcher opened (or re-focused) the editor, so restore it if minimised
    // and try to activate it. A freshly spawned plugin process may lack the
    // foreground rights (the host started it, not the user), and Windows can
    // refuse SetForegroundWindow; in that case flash the taskbar button
    // instead of stealing focus. No WS_EX_TOPMOST anywhere - once shown the
    // window behaves like any other and the user can put it behind.
    void showAndActivate()
    {
        if (!m_hwnd)
            return;
        if (IsIconic(m_hwnd))
            ShowWindow(m_hwnd, SW_RESTORE);
        else if (!IsWindowVisible(m_hwnd))
            ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        if (GetForegroundWindow() != m_hwnd) {
            FLASHWINFO fi {sizeof(FLASHWINFO), m_hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
            FlashWindowEx(&fi);
        }
    }

    // Called from another thread via PostMessage(WM_APP_FOCUS, , new OpenArgs*).
    void applyFocus(OpenArgs* a)
    {
        std::unique_ptr<OpenArgs> raii(a);
        // Re-focusing may happen after the host changed its language. Refresh
        // the live locale before anything is drawn from it.
        if (a->lang != m_args.lang || a->dark != m_dark) {
            m_args.setLang(a->lang);
            m_dark = a->dark;
            applyTheme();
            applyAppearanceText();
        }
        if (a->filter != m_args.filter) {
            m_args.filter = a->filter;
            m_filterText = a->filter;
            if (m_filterEdit)
                SetWindowTextW(m_filterEdit, wideFromUtf8(a->filter).c_str());
            rebuildVisible();
            m_topRow = 0;
            clampScroll();
            selectRow(0);
        }
        // A draft arriving through focus (e.g. from a launcher "env set") is shown
        // as an unsaved edit on top of the current read.
        if (!a->draft.isNull()) {
            loadDraft(a->draft, /*markDirty=*/true);
        }
        // Honour a requested selection if present.
        if (!a->select.empty())
            selectByRef(a->select);
        InvalidateRect(m_hwnd, nullptr, TRUE);
        // Re-focusing an existing editor is a user action too: bring it up
        // front (restore if minimised, flash if foreground rights are denied).
        showAndActivate();
    }

    // Posted by the protocol thread after an inline write (and by any other
    // writer). v0.6.0 routes through the same watcher as the 2s timer so the
    // diff is always built from base / mine / theirs, not just a yellow bar.
    void externalChange()
    {
        watchExternal();
    }

    void persistDraftIfDirty()
    {
        if (m_kind == "hosts" && m_tab == 1)
            commitRaw(); // a pending raw edit belongs in the draft too
        if (m_kind == "env" && m_envDirty())
            writeDraft(envDraft());
        else if (m_kind == "hosts" && m_hostsDirty())
            writeDraft(m_hosts.toDraft());
    }

private:
    OpenArgs m_args;
    std::string m_kind;
    HWND m_hwnd = nullptr;
    ThreadCtx* m_ctx = nullptr; // set before run(); used to report lifecycle to the protocol thread

    // model
    envh::edit::EnvModel m_env;
    bool m_hasEnv = false;
    envh::edit::HostsModel m_hosts;
    bool m_hasHosts = false;

    // ui state
    int m_tab = 0; // env: 0 user,1 system,2 backup | hosts: 0 entries,1 raw,2 backup
    int m_sel = 0; // index into m_visible (focus / anchor row)
    std::vector<int> m_visible; // indices into the model rows (filtered)
    // Multi-selection is keyed by MODEL row index, not by m_visible: filtering,
    // inserts and deletes reorder the view. m_sel stays the focused row.
    std::vector<std::uint8_t> m_selected;
    int m_rangeAnchor = 0; // visible index where Shift+range started
    int m_skippedSelected = 0; // read-only rows skipped by the last batch op
    std::vector<std::string> m_backupIds; // for the backup tab
    std::size_t m_backupTotal = 0; // independent of the page's scope filter
    std::string m_filterText;
    bool m_listMode = false;
    std::string m_listDelimiter = ";";
    std::map<std::string, std::string> m_delimPrefs; // per-variable applied delimiter
    std::vector<std::string> m_valueParts;
    int m_partSel = 0;
    // v0.7.1 multi-select for the item list. m_partMulti[i] is the visual
    // selection (Ctrl+Click toggles, Shift+Click ranges, ctrl+A selects all),
    // m_partSel stays the focus/caret row. The list box itself keeps
    // LBS_NOINTEGRALHEIGHT single-select semantics; the selection is ours.
    std::vector<char> m_partMulti;
    int m_partAnchor = -1; // Shift+Click range anchor
    int m_partsGuard = 0; // >0 while the item list is changed programmatically
    struct PartsGuard {
        EditorWindow* w;
        explicit PartsGuard(EditorWindow* win) : w(win) { ++w->m_partsGuard; }
        ~PartsGuard() { --w->m_partsGuard; }
    };
    int m_splitterY = 0;
    bool m_dragSplitter = false;
    bool m_extChanged = false;
    // ---- v0.6.0 state ----
    bool m_diffOpen = false; // read-only external-change diff overlay
    bool m_modalOpen = false; // a system MessageBox is up: suppress the watcher
    std::string m_extFingerprint; // last fingerprint the watcher diffed
    envh::edit::Diff3Result m_diff;
    envh::edit::Diff3HostsResult m_diffHosts;
    int m_diffScroll = 0;
    std::vector<std::string> m_partBase; // item snapshot when the row was opened
    int m_partInline = -1; // index being edited in place, -1 = none
    std::string m_partInlineOriginal; // rollback text for an in-place edit
    std::vector<char> m_partNew;      // per-item "added in this session" flags
    // Item candidates live outside the working model until Commit / Save.
    // Navigation retains the values and markers in m_partSessions.
    std::vector<char> m_partDeleted;  // per-item "staged for deletion" flags
    struct PartState {
        std::vector<std::string> values, base;
        std::vector<char> added, deleted, selected;
        std::string delimiter;
        int caret = 0, anchor = 0;
    };
    struct PartSession {
        std::string sourceValue;
        PartState current;
        std::vector<PartState> undo, redo;
    };
    std::map<std::string, PartSession> m_partSessions;
    std::string m_partRef, m_partSourceValue;
    PartState m_partInitial;
    RECT m_rcCommitBtn = {};          // green 提交修改 button in the detail pane
    RECT m_rcDiffPanel = {}, m_rcDiffReload = {}, m_rcDiffForce = {}, m_rcDiffDefer = {};
    // v0.7.2: the read-only footer hint of the diff panel. It used to be
    // derived from the button rect at paint time, which left it 2px tall.
    RECT m_rcDiffFooter = {};
    RECT m_rcBackupList = {}, m_rcBackupPreview = {}, m_rcBackupSearch = {};
    // v0.7.0: the backup page filters by registry scope. 0 = all, 1 = user,
    // 2 = system. The switch filters both the backup list and the preview.
    enum BackupScope { kScopeAll = 0, kScopeUser = 1, kScopeSystem = 2 };
    int m_backupScope = kScopeAll;
    RECT m_rcBackupScope[3] = {};
    RECT m_rcBackupScopeLabel = {};
    std::vector<int> m_backupCounts; // cached row count per backup id
    // Per-backup scope presence, so the list can show 用户/系统 chips.
    std::vector<char> m_backupHasUser, m_backupHasSystem;
    RECT m_rcBackupPreviewBtn = {}, m_rcBackupRestoreBtn = {}, m_rcBackupDeleteBtn = {}, m_rcBackupExportBtn = {};
    int m_backupPreviewSel = 0;
    bool m_dark = false;
    bool m_loading = false; // suppress EN_CHANGE while refreshDetail fills controls
    winshun::Json m_pendingDraft; // leftover draft awaiting the restore prompt

    // controls
    HWND m_filterEdit = nullptr;
    HWND m_nameEdit = nullptr, m_typeEdit = nullptr, m_valueEdit = nullptr, m_partsList = nullptr, m_partEdit = nullptr;
    HWND m_delimEdit = nullptr; // custom-delimiter box, visible only while m_delimCustom
    bool m_delimCustom = false;
    std::string m_pendingDelim; // a preset picked from the menu, applied immediately
    HWND m_enabledChk = nullptr, m_ipEdit = nullptr, m_hostEdit = nullptr, m_commentEdit = nullptr;
    HWND m_rawEdit = nullptr;
    HFONT m_editFont = nullptr;
    HFONT m_monoFont = nullptr; // hosts raw tab
    std::string m_rawShown; // raw tab text as last loaded (CRLF), to detect edits
    HBRUSH m_darkBgBrush = nullptr, m_darkFieldBrush = nullptr, m_lightPanelBrush = nullptr;
    int m_hover = 0;

    // Scrolling: m_topRow indexes m_visible (or m_backupIds on the backup tab).
    // The table is virtual: only [m_topRow, m_topRow + pageRows()) is painted.
    int m_topRow = 0;
    int m_topRowByTab[3] = {};

    // Command-level editing: one undo step per edit session, not per keystroke.
    bool m_editActive = false;

    // layout
    int m_clientW = 960, m_clientH = 620;
    RECT m_rcTabs[3] = {};
    RECT m_rcFilterLabel = {};
    RECT m_rcFilter = {};
    RECT m_rcTable = {};
    RECT m_rcDetail = {};
    RECT m_rcSplitter = {};
    RECT m_rcPartActions = {}; // v0.6.0: always empty (columns own the actions)
    RECT m_rcPartHeader = {};  // column header band above the item table
    // v0.7.1: the "备份 (N)" pill was removed; the count lives in the tab.
    RECT m_rcBandDiff = {}, m_rcBandReload = {}; // amber band actions
    ULONGLONG m_extSeenTick = 0; // GetTickCount64() when the change was seen
    ULONGLONG m_lastG = 0; // vim gg: tick of the previous bare 'g' keypress
    ULONGLONG m_lastD = 0; // vim dd: tick of the previous bare 'd' keypress
    int m_vimCount = -1; // vim count prefix (3j): -1 = no digits typed yet
    RECT m_rcModeRaw = {}, m_rcModeList = {}, m_rcDelimiter = {}, m_rcApply = {}, m_rcItemHint = {};
    RECT m_rcPartUp = {}, m_rcPartDown = {}, m_rcPartInsert = {}, m_rcPartDelete = {};
    RECT m_rcHostTitle = {}, m_rcHostEnabledLabel = {}, m_rcHostIpLabel = {};
    RECT m_rcHostNameLabel = {}, m_rcHostCommentLabel = {};
    RECT m_rcHostEnabled = {}, m_rcHostIp = {}, m_rcHostName = {}, m_rcHostComment = {};
    RECT m_rcStatus = {};
    RECT m_rcStatusRow1 = {}, m_rcStatusRow2 = {};
    RECT m_rcInfo = {}; // "ⓘ" permission hint, right end of status row 1
    RECT m_rcAddBtn = {}, m_rcDelBtn = {}, m_rcUndoBtn = {}, m_rcRedoBtn = {};
    struct Chip { int id; RECT rc; };
    std::vector<Chip> m_chips;
    int m_colX[6] = {}; // column boundaries, [0]=left .. [n]=right
    int m_colCount = 0;
    int m_rowH = 24;
    int m_headerH = 26;
    int m_statusFlashKind = 0; // 0 info, 1 warn, 2 error, 3 success
    ULONGLONG m_statusFlashUntil = 0;

    enum ChipId { kChipAdd = 1, kChipDel, kChipEdit, kChipUndo, kChipRedo, kChipSave, kChipClose,
                  kChipPartUp, kChipPartDown };
    RECT m_rcPartGroup = {};
    int m_envStateWidth = 80;
    static constexpr UINT kTimerDraft = 1;
    static constexpr UINT kTimerCommit = 2;
    static constexpr UINT kTimerStatus = 3;
    static constexpr UINT kTimerWatch = 4; // external target changed while open
    static constexpr int kBandH = 22; // height of one always-on notice band
    static constexpr const wchar_t* kClass = L"WinShunEnvHostsEditor";

    // ---- theme access ----------------------------------------------------
    // Every colour the painter uses comes from editor_theme.h, which is also
    // what tools/gen_design_svg.py parses to emit docs/design/*.svg. The
    // drafts and the product therefore share one definition and cannot drift.
    const envh::theme::Th& th() const { return *m_theme; }
    static envh::theme::Th& lightTheme()
    {
        static envh::theme::Th t = envh::theme::Th::light();
        return t;
    }
    static envh::theme::Th& darkTheme()
    {
        static envh::theme::Th t = envh::theme::Th::dark();
        return t;
    }
    void applyTheme()
    {
        m_theme = m_dark ? &darkTheme() : &lightTheme();
    }
    envh::theme::Th* m_theme = nullptr;
    // Metrics live in the same header, so a row-height change moves the
    // painter and the draft together.
    static constexpr int kThemeRowH = envh::theme::Metrics::kRowH;
    static constexpr int kThemeHeaderH = envh::theme::Metrics::kHeaderH;
    static constexpr int kThemeBandH = envh::theme::Metrics::kBandH;

    // ---- helpers ----
    static std::wstring wideFromUtf8(const std::string& t)
    {
        if (t.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, t.data(), static_cast<int>(t.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, t.data(), static_cast<int>(t.size()), w.data(), n);
        return w;
    }
    static std::string utf8FromWide(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
    static std::string formatCount(std::size_t n)
    {
        std::string s = std::to_string(n);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
            s.insert(static_cast<std::size_t>(i), ",");
        return s;
    }
    static HFONT makeFont(int h, bool bold, const wchar_t* face, DWORD pf)
    {
        return CreateFontW(-h, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pf, face);
    }
    static void ensureDpi()
    {
        if (const auto proc = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
            proc(reinterpret_cast<HANDLE>(-4));
        else if (GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware"))
            SetProcessDPIAware();
    }
    void registerClass()
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        RegisterClassW(&wc);
    }
    static DWORD windowStyle() { return (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_VSCROLL; }
    std::wstring title() const
    {
        return wideFromUtf8(m_kind == "env"
                                ? (m_args.zh ? "环境变量 — Win顺" : "Environment variables — WinShun")
                                : (m_args.zh ? "hosts — Win顺" : "hosts — WinShun"));
    }

    // The window owns a copy of the locale: the GUI thread must never call
    // back into the protocol thread's settings()/locale(). Both entry points
    // (a focus request and an appearance notification) go through here.
    void applyAppearanceText()
    {
        if (!m_hwnd)
            return;
        SetWindowTextW(m_hwnd, title().c_str());
        if (m_enabledChk)
            SetWindowTextW(m_enabledChk, m_args.zh ? L"启用" : L"Enabled");
        // A flash message was rendered in the old language; let the status bar
        // fall back to its normal (now translated) text.
        m_statusFlash.clear();
        m_statusFlashUntil = 0;
        computeLayout();
        applyLayout();
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    void applyAppearance(const AppearanceArgs& a)
    {
        m_args.setLang(a.lang);
        m_dark = a.dark;
        applyTheme();
        applyAppearanceText();
    }

    // ---- model load ----
    void loadModel()
    {
        if (m_kind == "env") {
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            const envh::native::EnvReadResult r = store.read(m_args.showSystem);
            m_env = envh::edit::EnvModel::fromSnapshot(r.snap);
            m_hasEnv = true;
        } else {
            envh::native::HostsStore store(m_args.hostsPath);
            const envh::native::HostsReadResult r = store.read();
            if (r.ok) {
                const envh::HostsParsed p = envh::parseHosts(r.content);
                m_hosts = envh::edit::HostsModel::fromParsed(p, dominantCrlf(p));
                m_hasHosts = true;
            }
        }
        // A leftover draft from a previous dirty close is NOT applied silently:
        // restoring it unseen made rows look locked (e.g. a draft that had
        // marked Path deleted). offerDraft() asks once the window is visible.
        const std::string draft = readDraft();
        if (!draft.empty()) {
            const auto j = winshun::Json::parse(draft);
            if (j && !j->isNull())
                m_pendingDraft = *j;
        }
        // The model holds rows, but the view indexes them through m_visible.
        // Without this the table is permanently empty.
        rebuildVisible();
        refreshBackups();
    }

    static std::map<std::string, PartSession> draftPartSessions(
        const winshun::Json& d, const envh::edit::EnvModel& model)
    {
        std::map<std::string, PartSession> sessions;
        for (const auto& entry : d["itemCandidates"].toArray()) {
            const std::string ref = entry["ref"].toString();
            const auto row = std::find_if(model.rows.begin(), model.rows.end(), [&](const auto& r) {
                return !r.deleted && r.scope + "/" + r.name == ref;
            });
            if (row == model.rows.end() || !entry["source"].isString()
                || entry["source"].toString() != row->value || !entry["values"].isArray())
                continue;
            PartSession s;
            s.sourceValue = row->value;
            s.current.delimiter = entry["delimiter"].toString();
            if (s.current.delimiter.empty())
                continue;
            bool valid = true;
            for (const auto& v : entry["values"].toArray()) {
                valid = valid && v.isString();
                s.current.values.push_back(v.toString());
            }
            if (!valid)
                continue;
            for (const auto& v : entry["base"].toArray())
                s.current.base.push_back(v.toString());
            for (const auto& v : entry["added"].toArray())
                s.current.added.push_back(v.toBool() ? 1 : 0);
            for (const auto& v : entry["deleted"].toArray())
                s.current.deleted.push_back(v.toBool() ? 1 : 0);
            s.current.added.resize(s.current.values.size());
            s.current.deleted.resize(s.current.values.size());
            s.current.selected.resize(s.current.values.size());
            if (!partPending(s))
                continue;
            // Restored candidates can be undone together back to their source.
            PartState initial;
            initial.delimiter = s.current.delimiter;
            initial.values = envh::edit::splitDelimited(s.sourceValue, initial.delimiter, true);
            initial.base = initial.values;
            initial.added.resize(initial.values.size());
            initial.deleted.resize(initial.values.size());
            initial.selected.resize(initial.values.size());
            s.undo.push_back(std::move(initial));
            sessions.emplace(ref, std::move(s));
        }
        return sessions;
    }

    void loadDraft(const winshun::Json& d, bool markDirty)
    {
        if (m_kind == "env") {
            clearPartSessions();
            m_env.fromDraft(d);
            m_partSessions = draftPartSessions(d, m_env);
        } else
            m_hosts.fromDraft(d);
        (void)markDirty; // fromDraft makes work differ from base => dirty
        clearSelection();
        rebuildVisible();
    }

    // Ask whether to restore the leftover draft. Yes applies it as unsaved
    // edits; No deletes the file so it is not offered again.
    void offerDraft()
    {
        if (m_pendingDraft.isNull())
            return;
        winshun::Json d = m_pendingDraft;
        m_pendingDraft = winshun::Json();
        std::size_t n = 0;
        if (m_kind == "env") {
            envh::edit::EnvModel probe = m_env;
            probe.fromDraft(d);
            n = probe.changeList().size() + draftPartSessions(d, probe).size();
        } else {
            envh::edit::HostsModel probe = m_hosts;
            probe.fromDraft(d);
            n = probe.changeList().size();
        }
        if (n == 0) {
            clearDraft();
            return;
        }
        const std::string msg = m_args.zh
            ? "发现上次未保存的修改（" + std::to_string(n) + " 项）。\n\n是：恢复这些修改（仍需 Ctrl+S 保存）\n否：丢弃草稿"
            : "Found " + std::to_string(n) + " unsaved change(s) from last time.\n\nYes: restore them (Ctrl+S still required)\nNo: discard the draft";
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"恢复草稿" : L"Restore draft";
        ds.subtitle = m_args.zh ? L"上次会话遗留的未保存修改" : L"Unsaved changes from the last session";
        ds.body = wideFromUtf8(msg);
        ds.icon = envh::dlg::kInfo;
        ds.buttons = {
            {m_args.zh ? L"丢弃" : L"Discard", IDNO, envh::dlg::kPlain},
            {m_args.zh ? L"恢复修改" : L"Restore", IDYES, envh::dlg::kPrimary},
        };
        ds.defButton = 1;
        const int r = envh::dlg::show(m_hwnd, ds, th(), m_dark);
        if (r == IDYES) {
            loadDraft(d, /*markDirty=*/true);
            selectRow(0);
            setStatus(m_args.zh ? "已恢复草稿，按 Ctrl+S 保存" : "Draft restored; Ctrl+S to save", 1);
        } else {
            clearDraft();
        }
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    std::string readDraft()
    {
        std::string error;
        if (!envh::drafts::migrate(m_args.dataDir, m_kind, error))
            winshun::debug("draft migration: " + error);
        const auto path = envh::drafts::path(m_args.dataDir, "gdi", m_kind);
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    void writeDraft(const winshun::Json& d)
    {
        const std::string dir = m_args.dataDir + "/drafts/gdi";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        std::ofstream out(std::filesystem::u8path(dir + "/" + m_kind + ".json"), std::ios::binary);
        if (out)
            out.write(d.dump().data(), static_cast<std::streamsize>(d.dump().size()));
    }
    void clearDraft()
    {
        std::error_code ec;
        std::filesystem::remove(envh::drafts::path(m_args.dataDir, "gdi", m_kind), ec);
    }

    bool m_envDirty() const { return m_hasEnv && (m_env.isDirty() || pendingPartChanges()); }
    bool m_hostsDirty() const { return m_hasHosts && m_hosts.isDirty(); }

    // Identity of the selected row, used to put the selection back after the
    // model is replaced by a fresh read.
    std::string currentRowRef() const
    {
        if (m_tab == 2 || m_visible.empty() || m_sel < 0 || m_sel >= static_cast<int>(m_visible.size()))
            return {};
        const int i = m_visible[m_sel];
        if (m_kind == "env")
            return m_env.rows[i].scope + "/" + m_env.rows[i].name;
        return std::to_string(m_hosts.rows[i].origin + 1);
    }

    // Replace the model with what is on disk right now, keeping the selection.
    // A reload is an explicit discard of the working copy, so the leftover draft
    // file goes with it; otherwise loadModel() would resurrect the edits the next
    // time the window opens.
    void reloadFromDisk()
    {
        const std::string ref = currentRowRef();
        clearPartSessions();
        clearDraft();
        clearSelection();
        m_skippedSelected = 0;
        if (m_kind == "env") {
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            m_env = envh::edit::EnvModel::fromSnapshot(store.read(m_args.showSystem).snap);
        } else {
            envh::native::HostsStore store(m_args.hostsPath);
            const envh::native::HostsReadResult r = store.read();
            if (r.ok) {
                const envh::HostsParsed p = envh::parseHosts(r.content);
                m_hosts = envh::edit::HostsModel::fromParsed(p, dominantCrlf(p));
            }
        }
        m_extChanged = false;
        rebuildVisible();
        if (ref.empty() || !selectByRef(ref))
            selectRow(m_sel);
        if (m_kind == "hosts" && m_tab == 1)
            loadRaw();
        applyLayout();
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    // The ending a new or rewritten line gets: whichever of CRLF / LF most
    // lines of the file already use (ties go to CRLF, the Windows default).
    static bool dominantCrlf(const envh::HostsParsed& p)
    {
        int crlf = 0, lf = 0;
        for (const auto& l : p.lines)
            (l.crlf ? crlf : lf) += 1;
        return crlf >= lf;
    }

    // F5 / "重新载入": never drop unsaved edits silently.
    void doReload()
    {
        endEdit();
        if (isDirty()) {
            const std::string body = m_args.zh ? "重新载入会丢弃未保存的修改，是否继续？"
                                               : "Reloading discards unsaved changes. Continue?";
            envh::dlg::Spec ds;
            ds.title = m_args.zh ? L"重新载入" : L"Reload";
            ds.subtitle = m_args.zh ? L"未保存的修改会丢失" : L"Unsaved changes will be lost";
            ds.body = wideFromUtf8(body);
            ds.icon = envh::dlg::kWarn;
            ds.buttons = {
                {m_args.zh ? L"取消" : L"Cancel", IDCANCEL, envh::dlg::kPlain},
                {m_args.zh ? L"重新载入" : L"Reload", IDYES, envh::dlg::kPrimary},
            };
            ds.defButton = 0;
            if (envh::dlg::show(m_hwnd, ds, th(), m_dark) != IDYES)
                return;
        }
        reloadFromDisk();
        setStatus(m_args.zh ? "已重新载入" : "Reloaded");
    }

    // ---- visible rows (tab + filter) ----
    void rebuildVisible()
    {
        m_visible.clear();
        if (m_kind == "env") {
            for (int i = 0; i < static_cast<int>(m_env.rows.size()); ++i) {
                const auto& r = m_env.rows[i];
                if (m_tab == 0 && r.scope != "user")
                    continue;
                if (m_tab == 1 && r.scope != "system")
                    continue;
                if (m_tab == 2)
                    continue;
                if (!m_filterText.empty()) {
                    const std::string lf = envh::toLower(m_filterText);
                    if (envh::toLower(r.name).find(lf) == std::string::npos
                        && envh::toLower(r.value).find(lf) == std::string::npos)
                        continue;
                }
                m_visible.push_back(i);
            }
        } else {
            for (int i = 0; i < static_cast<int>(m_hosts.rows.size()); ++i) {
                const auto& r = m_hosts.rows[i];
                if (m_tab == 2)
                    continue;
                if (!m_filterText.empty()) {
                    // Section dividers are visible as group headings when the
                    // filter is empty, but are hidden while filtering.
                    if (r.isSection)
                        continue;
                    const std::string lf = envh::toLower(m_filterText);
                    std::string hay = envh::toLower(r.ip);
                    for (const auto& h : r.hostnames)
                        hay += " " + envh::toLower(h);
                    hay += " " + envh::toLower(r.comment);
                    if (hay.find(lf) == std::string::npos)
                        continue;
                }
                m_visible.push_back(i);
            }
        }
        if (m_sel >= static_cast<int>(m_visible.size()))
            m_sel = static_cast<int>(m_visible.size()) - 1;
        if (m_sel < 0)
            m_sel = 0;
        clampScroll();
    }

    void selectRow(int idx)
    {
        stashPartView();
        m_lastG = m_lastD = 0;
        m_vimCount = -1;
        endEdit(); // changing rows ends the current edit session
        // Preserve candidates by their bound variable identity. Rebuilding a
        // view never applies candidate values to this or another model row.
        if (m_tab == 2) {
            const int n = static_cast<int>(m_backupIds.size());
            m_sel = n == 0 ? 0 : std::max(0, std::min(idx, n - 1));
            ensureVisible(m_sel);
            if (m_hwnd)
                InvalidateRect(m_hwnd, nullptr, TRUE);
            return;
        }
        if (m_visible.empty()) {
            m_sel = 0;
        } else {
            m_sel = std::max(0, std::min(idx, static_cast<int>(m_visible.size()) - 1));
            // Group headings are display-only. Landing on one would show an
            // empty detail pane, so move to the nearest real entry.
            if (m_kind == "hosts" && m_tab == 0) {
                auto heading = [&](int v) {
                    return v >= 0 && v < static_cast<int>(m_visible.size())
                           && m_hosts.rows[m_visible[v]].isSection;
                };
                if (heading(m_sel)) {
                    int v = m_sel + 1;
                    while (v < static_cast<int>(m_visible.size()) && heading(v))
                        ++v;
                    if (v >= static_cast<int>(m_visible.size())) {
                        v = m_sel - 1;
                        while (v >= 0 && heading(v))
                            --v;
                    }
                    if (v >= 0 && v < static_cast<int>(m_visible.size()))
                        m_sel = v;
                }
            }
        }
        ensureVisible(m_sel);
        refreshDetail();
        if (m_hwnd)
            InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    bool selectByRef(const std::string& ref)
    {
        auto selectModelRow = [&](int i) {
            rebuildVisible();
            for (std::size_t v = 0; v < m_visible.size(); ++v) {
                if (m_visible[v] == i) {
                    m_sel = static_cast<int>(v);
                    ensureVisible(m_sel);
                    refreshDetail();
                    return true;
                }
            }
            return false;
        };
        if (m_kind == "env") {
            for (int i = 0; i < static_cast<int>(m_env.rows.size()); ++i)
                if (m_env.rows[i].scope + "/" + m_env.rows[i].name == ref && selectModelRow(i))
                    return true;
        } else {
            // ref is a 1-based line number
            const int line = std::atoi(ref.c_str());
            for (int i = 0; i < static_cast<int>(m_hosts.rows.size()); ++i)
                if (m_hosts.rows[i].origin + 1 == line && selectModelRow(i))
                    return true;
        }
        return false;
    }

    // ---- keyboard focus chain -----------------------------------------------
    // Plain Tab moves through the visible detail fields and lands back on the
    // table; Ctrl+Tab is the only key that cycles the scope tabs, so the two
    // never compete for Tab.
    void moveFieldFocus(int dir)
    {
        std::vector<HWND> chain;
        if (m_kind == "env")
            chain = {m_nameEdit, m_typeEdit, m_delimEdit, m_valueEdit, m_partsList, m_partEdit};
        else
            chain = {m_enabledChk, m_ipEdit, m_hostEdit, m_commentEdit, m_rawEdit};
        std::vector<HWND> visibleFields;
        for (HWND h : chain)
            if (h && IsWindowVisible(h) && IsWindowEnabled(h))
                visibleFields.push_back(h);
        const HWND f = GetFocus();
        for (std::size_t i = 0; i < visibleFields.size(); ++i) {
            if (visibleFields[i] != f)
                continue;
            const long next = static_cast<long>(i) + dir;
            SetFocus(next < 0 || next >= static_cast<long>(visibleFields.size())
                         ? m_hwnd
                         : visibleFields[static_cast<std::size_t>(next)]);
            return;
        }
    }

    // ---- edit-control subclassing -------------------------------------------
    // Detail edit controls own the keyboard focus while text is being typed.
    // Without forwarding, Ins/Del/PgUp/PgDn/Ctrl+Z/Ctrl+S would be swallowed by
    // the EDIT and the window would never see them. Esc returns focus to table.
    struct EditSub { WNDPROC old; EditorWindow* self; };
    static std::mutex& editSubMutex() { static std::mutex m; return m; }
    static std::map<HWND, EditSub>& editSubs() { static std::map<HWND, EditSub> m; return m; }

    void subclassEdit(HWND e)
    {
        if (!e)
            return;
        WNDPROC old = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(e, GWLP_WNDPROC));
        {
            std::lock_guard<std::mutex> lk(editSubMutex());
            editSubs()[e] = {old, this};
        }
        SetWindowLongPtrW(e, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EditorWindow::editProc));
    }
    static LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        WNDPROC old = nullptr;
        EditorWindow* self = nullptr;
        {
            std::lock_guard<std::mutex> lk(editSubMutex());
            auto it = editSubs().find(h);
            if (it != editSubs().end()) {
                old = it->second.old;
                self = it->second.self;
            }
        }
        if (!old)
            return DefWindowProcW(h, msg, wp, lp);
        if (msg == WM_DESTROY) {
            SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(old));
            std::lock_guard<std::mutex> lk(editSubMutex());
            editSubs().erase(h);
            return CallWindowProcW(old, h, msg, wp, lp);
        }
        if (self && h == self->m_partsList && msg == WM_LBUTTONDOWN) {
            // v0.6.0 item table: the last column is the action cell. A click
            // there stages that row for deletion (rows with a state tag show
            // a label instead of ×, so nothing happens for them).
            const int x = static_cast<int>(LOWORD(lp));
            const int y = static_cast<int>(HIWORD(lp));
            DWORD pos = static_cast<DWORD>(y << 16 | (x & 0xFFFF));
            const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, static_cast<LPARAM>(pos));
            const int idx = static_cast<int>(LOWORD(hit));
            const bool outside = HIWORD(hit) != 0;
            if (!outside && idx >= 0 && idx < static_cast<int>(self->m_valueParts.size())
                && self->partActionHit(x)) {
                // v0.7.0 two-stage delete: x stages a row (red, "− 待删除")
                // and x on a staged row RESTORES it. Added rows still vanish
                // at once - they never existed in the value.
                self->commitPartEdit();
                if (self->m_partInline >= 0)
                    self->endPartInline(true);
                self->m_partSel = idx;
                self->partSelectionClear(); // the row's own x targets that row
                self->showCurrentPart();
                SetFocus(h);
                self->deletePart();
                return 0;
            }
            // v0.7.1 multi-select: Ctrl+Click toggles, Shift+Click ranges.
            // The modifiers come from MK_* in wParam (injected mouse messages
            // do not update GetKeyState, the WM_APP_KEY lesson again); fall
            // back to the real key state for physical clicks.
            const bool ctrl = (wp & MK_CONTROL) != 0
                || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (wp & MK_SHIFT) != 0
                || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!outside && (ctrl || shift)) {
                self->commitPartEdit();
                if (self->m_partInline >= 0)
                    self->endPartInline(true);
                self->partSelectionGesture(idx, ctrl, shift);
                self->showCurrentPart();
                SetFocus(h);
                return 0; // keep the list box from moving its own caret
            }
        }
        if (self && h == self->m_partsList && msg == WM_LBUTTONDBLCLK) {
            const int y = static_cast<int>(HIWORD(lp));
            const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0,
                                             static_cast<LPARAM>(y << 16));
            const int idx = static_cast<int>(LOWORD(hit));
            if (HIWORD(hit) == 0)
                self->beginPartInline(idx);
            return 0;
        }
        if (self && h == self->m_rawEdit) {
            if (msg == WM_CHAR && wp == L'\t') {
                // Insert a real tab; hosts files use tabs as column separators.
                SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\t"));
                return 0;
            }
            if (msg == WM_CHAR && (wp == 0x13 || wp == 0x01 || wp == 0x19))
                return 0; // swallow the Ctrl+S / Ctrl+A / Ctrl+Y control characters (beep)
            if (msg == WM_LBUTTONUP || msg == WM_KEYUP) {
                const LRESULT r = CallWindowProcW(old, h, msg, wp, lp);
                InvalidateRect(self->m_hwnd, &self->m_rcStatusRow1, FALSE);
                return r;
            }
        }
        if (self && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (h == self->m_rawEdit) {
                // The raw box is a text editor: its own Ctrl+Z / Ctrl+Y, arrows,
                // PgUp/PgDn, Del and Enter stay native. Only window-level keys
                // leave it. Ctrl+A is added because a plain EDIT lacks it.
                if (ctrl && (wp == 'S' || wp == 's')) { self->save(); return 0; }
                if (ctrl && (wp == 'A' || wp == 'a')) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
                if (ctrl && (wp == 'Y' || wp == 'y')) {
                    // EDIT has a single-level undo where a second EM_UNDO redoes.
                    if (SendMessageW(h, EM_CANUNDO, 0, 0))
                        SendMessageW(h, EM_UNDO, 0, 0);
                    return 0;
                }
                if (ctrl && wp == VK_TAB) return self->onKey(wp);
                if (wp == VK_ESCAPE && !ctrl) { PostMessageW(self->m_hwnd, WM_CLOSE, 0, 0); return 0; }
                const LRESULT r = CallWindowProcW(old, h, msg, wp, lp);
                InvalidateRect(self->m_hwnd, &self->m_rcStatusRow1, FALSE); // caret line
                return r;
            }
            if (h == self->m_filterEdit) {
                // Esc in the filter clears it and returns focus to the table;
                // Ctrl+Z must stay a native text undo here.
                if (wp == VK_ESCAPE && !ctrl) {
                    if (!self->m_filterText.empty()) {
                        self->clearFilter();
                        self->rebuildVisible();
                        self->m_topRow = 0;
                        self->clampScroll();
                        self->selectRow(0);
                        InvalidateRect(self->m_hwnd, nullptr, TRUE);
                    }
                    SetFocus(self->m_hwnd);
                    return 0;
                }
                if (wp == VK_TAB) {
                    if (ctrl)
                        return self->onKey(wp);
                    SetFocus(self->m_hwnd);
                    return 0;
                }
                return CallWindowProcW(old, h, msg, wp, lp);
            }
            if (h == self->m_partEdit) {
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                if ((ctrl && (wp == 'N' || wp == 'J' || wp == 'K' || wp == 'U'
                              || wp == 'S' || wp == 'Z' || wp == 'Y' || wp == 'R'
                              || wp == VK_TAB))
                    || (alt && (wp == 'J' || wp == 'K' || wp == VK_UP || wp == VK_DOWN)))
                    return self->onKey(wp);
                // The floating in-place edit box sits over one LISTBOX row.
                // Enter commits, Esc restores the text captured on open, and a
                // Tab commits before moving on. EN_CHANGE already writes the
                // text into m_valueParts live via commitPartEdit().
                if (wp == VK_RETURN && !ctrl) { self->endPartInline(true); return 0; }
                if (wp == VK_ESCAPE && !ctrl) { self->endPartInline(false); return 0; }
                if (wp == VK_TAB) { self->endPartInline(true); return 0; }
                return CallWindowProcW(old, h, msg, wp, lp);
            }
            if (h == self->m_delimEdit) {
                if (wp == VK_RETURN && !ctrl) {
                    self->applyDelimiter();
                    return 0;
                }
                if (wp == VK_ESCAPE && !ctrl) {
                    self->syncDelimiterComboText();
                    SetFocus(self->m_hwnd);
                    return 0;
                }
                if (wp == VK_TAB) {
                    if (ctrl)
                        return self->onKey(wp);
                    self->moveFieldFocus(shift ? -1 : 1);
                    return 0;
                }
                return CallWindowProcW(old, h, msg, wp, lp);
            }
            if (wp == VK_ESCAPE && !ctrl) {
                self->endEdit();
                SetFocus(self->m_hwnd);
                return 0;
            }
            // Plain Tab walks the detail fields; only Ctrl+Tab reaches the
            // parent handler, which uses it to cycle the scope tabs.
            if (wp == VK_TAB && !ctrl) {
                self->moveFieldFocus(shift ? -1 : 1);
                return 0;
            }
            if ((wp == VK_TAB && ctrl) || wp == VK_F5
                || (ctrl && (wp == 'Z' || wp == 'z' || wp == 'Y' || wp == 'y'
                              || wp == 'S' || wp == 's' || wp == 'R' || wp == 'r'))) {
                return self->onKey(wp);
            }
            // Ins / Del / PgUp / PgDn belong to the focused control. Forwarding
            // them from an EDIT to the table made Del mark the whole row deleted
            // while the user was deleting a character - the Path variable then
            // vanished on save. Only the parts LISTBOX uses them as commands.
            // v0.7.1 keymap: Del/Backspace stage the selected items; plain
            // Up/Down stay NATIVE (the list box moves its own caret and fires
            // LBN_SELCHANGE, which syncs m_partSel); Alt+Up/Down must reach
            // onKey for the reorder, Ctrl+N/J/K/U are forwarded explicitly.
            if (h == self->m_partsList) {
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (wp == VK_INSERT || wp == VK_DELETE || wp == VK_BACK)
                    return self->onKey(wp);
                if (alt && (wp == VK_UP || wp == VK_DOWN || wp == 'J' || wp == 'K'))
                    return self->onKey(wp);
                if (ctrl && (wp == 'N' || wp == 'n' || wp == 'J' || wp == 'j'
                             || wp == 'K' || wp == 'k'))
                    return self->onKey(wp);
                // List commands stay out of text fields and native type-to-select.
                if (!alt && !ctrl
                    && (wp == 'J' || wp == 'K' || wp == 'G' || wp == 'D'
                        || wp == 'X' || wp == 'U' || (wp >= '0' && wp <= '9')))
                    return self->onKey(wp);
            }
        }
        if (self && h == self->m_partsList && msg == WM_CHAR
            && (wp == 'j' || wp == 'k' || wp == 'g' || wp == 'G' || wp == 'd'
                || wp == 'x' || wp == 'u' || (wp >= '0' && wp <= '9')))
            return 0;
        return CallWindowProcW(old, h, msg, wp, lp);
    }

    // ---- controls ----
    void createControls()
    {
        // Detail controls for both kinds are created up front. They must start
        // hidden: applyLayout() shows and positions only the group belonging to
        // the current kind. Visible controls left at the placeholder (0,0,10,20)
        // would otherwise paint stray empty boxes in the top-left corner.
        const DWORD es = WS_CHILD | WS_TABSTOP;
        m_filterEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", es | ES_AUTOHSCROLL, 0, 0, 10, 20, m_hwnd,
                                       reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        if (m_filterEdit) {
            if (m_editFont)
                SendMessageW(m_filterEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
            m_loading = true;
            SetWindowTextW(m_filterEdit, wideFromUtf8(m_args.filter).c_str());
            m_loading = false;
            ShowWindow(m_filterEdit, SW_SHOW);
            subclassEdit(m_filterEdit);
        }

        // env detail
        m_nameEdit = makeEdit(es, 2);
        m_typeEdit = makeEdit(es, 3);
        m_valueEdit = makeEdit(es | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 4);
        // The default EDIT text limit silently truncates a long value on
        // SetWindowTextW. An overlong PATH used to come back from the raw box
        // already cut short, so switching to item mode committed the truncated
        // text into the model. Both env boxes must hold the full registry value;
        // validation (kMaxEnvValue) is what rejects it, not the control.
        SendMessageW(m_valueEdit, EM_SETLIMITTEXT, 0, 0);
        m_partsList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                     es | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED
                                         | LBS_HASSTRINGS | WS_VSCROLL,
                                     0, 0, 10, 20, m_hwnd,
                                     reinterpret_cast<HMENU>(10), GetModuleHandleW(nullptr), nullptr);
        if (m_partsList && m_editFont) {
            SendMessageW(m_partsList, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
            SendMessageW(m_partsList, LB_SETITEMHEIGHT, 0, 22);
        }
        // The delimiter is a self-drawn dropdown button (painted like 应用 and
        // the mode segment). This flat EDIT only appears for "自定义…".
        m_delimEdit = CreateWindowExW(0, L"EDIT", L"", es | WS_BORDER | ES_AUTOHSCROLL,
                                      0, 0, 10, 20, m_hwnd,
                                      reinterpret_cast<HMENU>(12), GetModuleHandleW(nullptr), nullptr);
        if (m_delimEdit && m_editFont)
            SendMessageW(m_delimEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
        subclassEdit(m_delimEdit);
        m_partEdit = makeEdit(es | ES_AUTOHSCROLL, 11);
        SendMessageW(m_partEdit, EM_SETLIMITTEXT, 0, 0);
        subclassEdit(m_partsList);
        // hosts detail
        m_enabledChk = CreateWindowExW(0, L"BUTTON", m_args.zh ? L"启用" : L"Enabled",
                                       WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 16, 16,
                                       m_hwnd, reinterpret_cast<HMENU>(5), GetModuleHandleW(nullptr), nullptr);
        // Without WM_SETFONT a BUTTON keeps the bold bitmap System font.
        if (m_enabledChk && m_editFont)
            SendMessageW(m_enabledChk, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
        subclassEdit(m_enabledChk);
        m_ipEdit = makeEdit(es, 6);
        m_hostEdit = makeEdit(es, 7);
        m_commentEdit = makeEdit(es, 8);
        // hosts raw tab
        m_rawEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL
                                        | ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL,
                                    0, 0, 10, 10,
                                    m_hwnd, reinterpret_cast<HMENU>(9), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(m_rawEdit, EM_SETLIMITTEXT, 0, 0); // hosts files can exceed the 30k default
        // Monospace so IP / host columns line up as they do in the file.
        m_monoFont = makeFont(14, false, L"Consolas", FIXED_PITCH | FF_MODERN);
        if (m_rawEdit)
            SendMessageW(m_rawEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_monoFont ? m_monoFont : m_editFont), TRUE);
        subclassEdit(m_rawEdit);
    }

    HWND makeEdit(DWORD style, int id)
    {
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style, 0, 0, 10, 20, m_hwnd,
                                 reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
        if (e && m_editFont)
            SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
        subclassEdit(e);
        return e;
    }

    void computeLayout(int cw = 0, int ch = 0)
    {
        if (cw > 0 && ch > 0) {
            m_clientW = cw;
            m_clientH = ch;
        }
        int x = 8;
        for (int i = 0; i < 3; ++i) {
            const int w = 84;
            m_rcTabs[i] = {x, 4, x + w, 28};
            x += w + 4;
        }
        // Reserve a strip for a filter caption drawn on the parent DC. Drawing
        // the hint inside the edit control's rect does not work: the real EDIT
        // child window paints over it.
        m_rcFilterLabel = {x, 4, x + 82, 28};
        m_rcFilter = {x + 82, 5, m_clientW - 12, 27};
        buildChips(); // measure and reserve all footer rows before laying out panes
        const int statusH = m_clientH - m_rcStatus.top;
        const int workTop = 34;
        const int workBottom = m_clientH - statusH;
        const int minListH = 112, minDetailH = 200, splitterH = 7;
        const int fixedDetailH = 112;
        // Only the env list/detail split is resizable; the hosts editor and the
        // backup tab keep their original fixed detail strip.
        const bool resizable = (m_kind == "env" && m_tab != 2);
        if (resizable) {
            if (m_splitterY == 0)
                m_splitterY = workBottom - minDetailH - splitterH;
            m_splitterY = std::max(workTop + minListH,
                                   std::min(m_splitterY, workBottom - minDetailH - splitterH));
            m_rcTable = {8, workTop, m_clientW - 8, m_splitterY};
            m_rcDetail = {8, m_splitterY + splitterH, m_clientW - 8, workBottom};
            m_rcSplitter = {8, m_splitterY, m_clientW - 8, m_splitterY + splitterH};
        } else {
            // The hosts raw tab has no detail strip: the text box runs down to
            // the status bar instead of leaving 112px empty below it.
            const bool rawTab = m_kind == "hosts" && m_tab == 1;
            const int detailH = m_kind == "hosts" && m_tab == 0 ? 146 : fixedDetailH;
            m_rcTable = {8, workTop, m_clientW - 8, rawTab ? workBottom - 8 : workBottom - detailH - 8};
            m_rcDetail = rawTab ? RECT {8, workBottom, m_clientW - 8, workBottom}
                                : RECT {8, workBottom - detailH, m_clientW - 8, workBottom};
            SetRectEmpty(&m_rcSplitter);
        }

        // The mode segment, delimiter box and apply action share one toolbar row.
        // Their widths shrink with the pane, while a visible-item hint takes the
        // remaining space. This keeps the Chinese and English labels in the same
        // visual band as the existing action buttons.
        const int lx = m_rcDetail.left + 4;
        const int modeH = 22;
        const int modeTop = m_rcDetail.top + 48; // below the name/type row (top+20..42)
        // v0.7.2: the item counter needs a guaranteed strip. The right-hand
        // reserve used to be a flat 208px, of which only the 108px commit
        // button was real, and the delimiter box took every remaining pixel -
        // the counter was left with 80px, so "共 1 项 · 合计 " was clipped away
        // and only "9 / 32,767 字符" survived. The delimiter box (the only
        // elastic element) now yields to a fixed minimum for the counter.
        const int commitW = 108;   // 提交修改
        const int hintMinW = 230;  // "共 N 项 · 合计 X / 32,767 字符"
        const int kGaps = 6 + 6 + 8; // mode|delim, delim|apply, apply|hint
        const int commitRight = m_rcDetail.right - 4;
        const int commitLeft = commitRight - commitW;
        const int strip = std::max(160, commitLeft - 8 - lx); // room for mode..hint
        const int modeW = std::max(132, std::min(180, strip * 30 / 100));
        const int applyW = std::max(54, std::min(72, strip * 14 / 100));
        const int rest = std::max(0, strip - modeW - applyW - kGaps);
        const int comboW = std::max(92, std::min(300, rest - hintMinW));
        const int hintW = std::max(0, rest - comboW);
        int cx = lx;
        const int cellW = modeW / 2;
        m_rcModeRaw = {cx, modeTop, cx + cellW, modeTop + modeH};
        m_rcModeList = {cx + cellW, modeTop, cx + modeW, modeTop + modeH};
        cx += modeW + 6;
        m_rcDelimiter = {cx, modeTop, cx + comboW, modeTop + modeH};
        cx += comboW + 6;
        m_rcApply = {cx, modeTop, cx + applyW, modeTop + modeH};
        cx += applyW + 8;
        // v0.7.0 two-stage delete: the green 提交修改 button sits at the right
        // end of the item toolbar and applies staged item deletions.
        m_rcCommitBtn = {commitLeft, modeTop, commitRight, modeTop + modeH};
        // The counter sits directly after the "+ 新增项" button, as in the
        // design draft, and is left-aligned so nothing is clipped away.
        m_rcItemHint = {cx, modeTop, cx + hintW, modeTop + modeH};

        if (m_kind == "hosts" && m_tab == 0) {
            const int innerLeft = m_rcDetail.left + 12;
            const int innerRight = m_rcDetail.right - 12;
            const int innerW = innerRight - innerLeft;
            const int statusW = std::max(72, innerW * 14 / 100);
            const int ipW = innerW * 22 / 100;
            const int hostW = innerW * 38 / 100;
            const int labelY = m_rcDetail.top + 40;
            const int fieldY = m_rcDetail.top + 58;
            m_rcHostTitle = {innerLeft, m_rcDetail.top + 8, innerLeft + 220, m_rcDetail.top + 30};
            m_rcHostEnabledLabel = {innerLeft, labelY, innerLeft + statusW, labelY + 16};
            m_rcHostIpLabel = {innerLeft + statusW + 8, labelY,
                               innerLeft + statusW + ipW, labelY + 16};
            m_rcHostNameLabel = {innerLeft + statusW + ipW + 8, labelY,
                                 innerLeft + statusW + ipW + hostW, labelY + 16};
            m_rcHostCommentLabel = {innerLeft + statusW + ipW + hostW + 8, labelY,
                                    innerRight, labelY + 16};
            m_rcHostEnabled = {innerLeft, fieldY, innerLeft + statusW, fieldY + 26};
            m_rcHostIp = {innerLeft + statusW + 8, fieldY,
                          innerLeft + statusW + ipW, fieldY + 26};
            m_rcHostName = {innerLeft + statusW + ipW + 8, fieldY,
                            innerLeft + statusW + ipW + hostW, fieldY + 26};
            m_rcHostComment = {innerLeft + statusW + ipW + hostW + 8, fieldY,
                               innerRight, fieldY + 26};
            const int bw = 60, gap = 4;
            int bx = m_rcDetail.right - 4 * bw - 3 * gap - 12;
            const int by = m_rcDetail.top + 7;
            m_rcAddBtn = {bx, by, bx + bw, by + 26}; bx += bw + gap;
            m_rcDelBtn = {bx, by, bx + bw, by + 26}; bx += bw + gap;
            m_rcUndoBtn = {bx, by, bx + bw, by + 26}; bx += bw + gap;
            m_rcRedoBtn = {bx, by, bx + bw, by + 26};
        } else {
            const int bw = 46, gap = 4;
            int bx = m_rcDetail.right - 4 * bw - 3 * gap - 4;
            const int by = m_rcDetail.top + 20;
            m_rcAddBtn = {bx, by, bx + bw, by + 22}; bx += bw + gap;
            m_rcDelBtn = {bx, by, bx + bw, by + 22}; bx += bw + gap;
            m_rcUndoBtn = {bx, by, bx + bw, by + 22}; bx += bw + gap;
            m_rcRedoBtn = {bx, by, bx + bw, by + 22};
        }
        // v0.7.1: the "备份 (N)" pill is gone - the backup count lives in the
        // top tab strip now ("备份 (N)"), so no detail-header control here.

        // Column boundaries are proportional to the table width so the layout
        // stays correct when the window is resized (WM_SIZE triggers recompute).
        const int L = m_rcTable.left, W = m_rcTable.right - L;
        if (m_kind == "env") {
            // Design columns: name | type | value | status. The status column
            // carries the same + / ● / − vocabulary as the row bar.
            m_colCount = 4;
            m_colX[0] = L;
            m_colX[1] = L + W * 28 / 100;
            m_colX[2] = L + W * 43 / 100;
            m_colX[3] = m_rcTable.right - m_envStateWidth;
            m_colX[4] = m_rcTable.right;
        } else {
            m_colCount = 5;
            m_colX[0] = L;
            m_colX[1] = L + W * 8 / 100;
            m_colX[2] = L + W * 28 / 100;
            m_colX[3] = L + W * 62 / 100;
            m_colX[4] = m_rcTable.right - W * 10 / 100;
            m_colX[5] = m_rcTable.right;
        }
        // v0.6.0: centred read-only diff overlay and the backup two-pane split.
        const int pw = std::min(760, m_clientW - 40);
        const int ph = std::min(440, m_clientH - 80);
        const int px = (m_clientW - pw) / 2, py = (m_clientH - ph) / 2;
        m_rcDiffPanel = {px, py, px + pw, py + ph};
        const int bw = (pw - 40) / 3;
        // v0.7.2: the footer hint needs a band of its own. The buttons used to
        // end 12px above the panel edge, so the hint rect collapsed to 2px and
        // the panel border cut the text in half.
        const int by = py + ph - 64;
        m_rcDiffReload = {px + 12, by, px + 12 + bw, by + 28};
        m_rcDiffForce = {px + 16 + bw, by, px + 16 + 2 * bw, by + 28};
        m_rcDiffDefer = {px + 20 + 2 * bw, by, px + 20 + 3 * bw, by + 28};
        m_rcDiffFooter = {px + 18, py + ph - 30, px + pw - 18, py + ph - 8};
        const int blW = std::max(190, static_cast<int>(m_rcTable.right - m_rcTable.left) * 30 / 100);
        m_rcBackupList = {m_rcTable.left, m_rcTable.top, m_rcTable.left + blW, m_rcTable.bottom};
        m_rcBackupPreview = {m_rcTable.left + blW + 8, m_rcTable.top,
                             m_rcTable.right, m_rcTable.bottom};
        // The scope switch sits in the tab strip, flush against the window's
        // right edge (the filter edit lives inside the preview pane on this
        // tab, so the top-right corner is free). It filters the backup list
        // AND the preview, so it belongs to the page header.
        const int ssW = 52, ssH = 24, ssY = m_rcTabs[2].top + 2;
        const int ssRight = m_clientW - 12;
        m_rcBackupScope[2] = {ssRight - ssW, ssY, ssRight, ssY + ssH};
        m_rcBackupScope[1] = {m_rcBackupScope[2].left - ssW - 4, ssY,
                              m_rcBackupScope[2].left - 4, ssY + ssH};
        m_rcBackupScope[0] = {m_rcBackupScope[1].left - ssW - 4, ssY,
                              m_rcBackupScope[1].left - 4, ssY + ssH};
        m_rcBackupScopeLabel = {m_rcBackupScope[0].left - 62, ssY,
                                m_rcBackupScope[0].left - 6, ssY + ssH};
        // The search box sits BETWEEN the pane header and the column header;
        // it used to overlap the header band (both at top+52).
        m_rcBackupSearch = {m_rcBackupPreview.left + 8, m_rcBackupPreview.top + 28,
                            m_rcBackupPreview.left + 308, m_rcBackupPreview.top + 52};
        const int bbW = 104, bbY = m_rcDetail.top + 10;
        m_rcBackupPreviewBtn = {m_rcDetail.left + 10, bbY, m_rcDetail.left + 10 + bbW, bbY + 26};
        m_rcBackupRestoreBtn = {m_rcDetail.left + 14 + bbW, bbY,
                                m_rcDetail.left + 14 + 2 * bbW, bbY + 26};
        m_rcBackupDeleteBtn = {m_rcDetail.left + 18 + 2 * bbW, bbY,
                               m_rcDetail.left + 18 + 3 * bbW, bbY + 26};
        m_rcBackupExportBtn = {m_rcDetail.left + 22 + 3 * bbW, bbY,
                               m_rcDetail.left + 22 + 4 * bbW, bbY + 26};
        clampScroll();
    }

    void buildChips()
    {
        m_chips.clear();
        SetRectEmpty(&m_rcPartUp);
        SetRectEmpty(&m_rcPartDown);
        SetRectEmpty(&m_rcPartGroup);
        HDC dc = GetDC(m_hwnd);
        HFONT font = makeFont(11, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        HGDIOBJ old = SelectObject(dc, font);
        TEXTMETRICW tm {};
        GetTextMetricsW(dc, &tm);
        const int h = std::max(24, static_cast<int>(tm.tmHeight) + 8);
        const auto textWidth = [&](const wchar_t* label) {
            SIZE size {};
            GetTextExtentPoint32W(dc, label, lstrlenW(label), &size);
            return static_cast<int>(size.cx);
        };
        m_envStateWidth = 80;
        for (auto state : {envh::edit::RowState::Unchanged, envh::edit::RowState::Added,
                           envh::edit::RowState::Modified, envh::edit::RowState::Deleted})
            for (bool pending : {false, true})
                m_envStateWidth = std::max(m_envStateWidth, textWidth(envStateLabel(state, pending)) + 20);
        const auto chipWidth = [&](int id) {
            int w = textWidth(chipLabel(id));
            if (id == kChipDel) {
                for (const wchar_t* variant : {m_args.zh ? L"Ctrl+U 删除整行" : L"Ctrl+U del row",
                                              m_args.zh ? L"Del 恢复" : L"Del restore"})
                    w = std::max(w, textWidth(variant));
            }
            return w + 20;
        };
        int x = 8, y = 4;
        const auto group = [&](std::initializer_list<int> ids, const wchar_t* label) {
            const int labelW = *label ? textWidth(label) + 8 : 0;
            int width = labelW - 6;
            for (int id : ids) width += chipWidth(id) + 6;
            if (x + width > m_clientW - 8 && x > 8) {
                x = 8;
                y += h + 4;
            }
            if (labelW) {
                m_rcPartGroup = {x, y, x + labelW - 8, y + h};
                x += labelW;
            }
            for (int id : ids) {
                const int w = chipWidth(id);
                m_chips.push_back({id, {x, y, x + w, y + h}});
                x += w + 6;
            }
            x += 10;
        };
        group({kChipAdd, kChipDel, kChipEdit, kChipUndo, kChipRedo}, L"");
        if (m_kind == "env" && m_tab != 2 && m_listMode)
            group({kChipPartUp, kChipPartDown}, m_args.zh ? L"分隔项：" : L"Items:");
        group({kChipSave, kChipClose}, L"");
        // Stable action indices for read-only geometry probes and callers.
        std::sort(m_chips.begin(), m_chips.end(), [](const Chip& a, const Chip& b) { return a.id < b.id; });
        SelectObject(dc, old);
        DeleteObject(font);
        ReleaseDC(m_hwnd, dc);
        const int top = m_clientH - (y + h + 4);
        for (auto& chip : m_chips)
            OffsetRect(&chip.rc, 0, top);
        if (m_rcPartGroup.right > m_rcPartGroup.left)
            OffsetRect(&m_rcPartGroup, 0, top);
        for (const auto& chip : m_chips) {
            if (chip.id == kChipPartUp) m_rcPartUp = chip.rc;
            if (chip.id == kChipPartDown) m_rcPartDown = chip.rc;
        }
        m_rcStatusRow2 = {0, top, m_clientW, m_clientH};
        m_rcStatusRow1 = {0, top - 24, m_clientW, top};
        m_rcStatus = {0, top - 24, m_clientW, m_clientH};
    }

    // ---- virtual list --------------------------------------------------------
    int rowCount() const
    {
        return m_tab == 2 ? static_cast<int>(m_backupIds.size())
                          : static_cast<int>(m_visible.size());
    }
    int pageRows() const
    {
        const int avail = m_rcTable.bottom - m_rcTable.top
                          - (m_tab == 2 ? 4 : m_headerH) - bannerH();
        return std::max(1, avail / rowH());
    }
    // Per-tab row height: the backup list uses 44px two-line entries, every
    // other list uses the standard single-line height.
    int rowH() const
    {
        return m_tab == 2 ? envh::theme::Metrics::kBackupListRowH : m_rowH;
    }
    int maxTopRow() const
    {
        return std::max(0, rowCount() - pageRows());
    }
    void clampScroll()
    {
        if (m_topRow > maxTopRow())
            m_topRow = maxTopRow();
        if (m_topRow < 0)
            m_topRow = 0;
        m_topRowByTab[m_tab] = m_topRow;
        syncScrollbar();
    }
    void syncScrollbar()
    {
        if (!m_hwnd)
            return;
        // The raw text box scrolls itself; a second, window-level bar sized
        // for the entry list would sit next to it.
        if (m_kind == "hosts" && m_tab == 1) {
            ShowScrollBar(m_hwnd, SB_VERT, FALSE);
            return;
        }
        ShowScrollBar(m_hwnd, SB_VERT, TRUE);
        SCROLLINFO si {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        si.nMin = 0;
        si.nMax = std::max(0, rowCount() - 1);
        si.nPage = static_cast<UINT>(pageRows());
        si.nPos = m_topRow;
        SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
    }
    void scrollTo(int top)
    {
        const int t = std::max(0, std::min(top, maxTopRow()));
        if (t != m_topRow) {
            m_topRow = t;
            m_topRowByTab[m_tab] = t;
            if (m_hwnd)
                InvalidateRect(m_hwnd, nullptr, TRUE);
        }
        syncScrollbar();
    }
    void ensureVisible(int sel)
    {
        if (sel < 0 || rowCount() == 0) {
            clampScroll();
            return;
        }
        if (sel < m_topRow)
            scrollTo(sel);
        else if (sel >= m_topRow + pageRows())
            scrollTo(sel - pageRows() + 1);
        else
            syncScrollbar();
    }

    void applyLayout()
    {
        // The vertical splitter exists only in the env PATH item mode. Clear it
        // first so switching kind/tab/mode cannot leave a stale hit target.
        // Keep the two detail groups mutually exclusive. The other kind's
        // controls would otherwise stay at (0,0) as empty framed boxes.
        if (m_kind == "env") {
            for (HWND e : {m_enabledChk, m_ipEdit, m_hostEdit, m_commentEdit})
                if (e) ShowWindow(e, SW_HIDE);
        } else {
            for (HWND e : {m_nameEdit, m_typeEdit, m_valueEdit})
                if (e) ShowWindow(e, SW_HIDE);
        }
        if (m_filterEdit) {
            ShowWindow(m_filterEdit, SW_SHOW);
            // On the backup page the same edit box doubles as the preview
            // search, and the design puts that search inside the preview pane.
            const RECT fr = (m_tab == 2 && m_rcBackupSearch.right > m_rcBackupSearch.left)
                                ? m_rcBackupSearch
                                : m_rcFilter;
            MoveWindow(m_filterEdit, fr.left, fr.top, fr.right - fr.left, fr.bottom - fr.top, TRUE);
        }
        const bool showDetail = !(m_tab == 2);
        const bool raw = (m_kind == "hosts" && m_tab == 1);
        if (m_rawEdit) {
            ShowWindow(m_rawEdit, raw ? SW_SHOW : SW_HIDE);
            if (raw) {
                MoveWindow(m_rawEdit, m_rcTable.left, m_rcTable.top,
                           m_rcTable.right - m_rcTable.left, m_rcTable.bottom - m_rcTable.top, TRUE);
                // Writable hosts: the raw text is a second way to edit the file.
                // Read-only (no admin) or a refused UTF-16 file keeps it locked.
                SendMessageW(m_rawEdit, EM_SETREADONLY, rawEditable() ? FALSE : TRUE, 0);
            }
        }
        if (m_partsList) {
            ShowWindow(m_partsList, SW_HIDE);
            if (m_kind == "env" && m_listMode && showDetail && !raw)
                ShowWindow(m_partsList, SW_SHOW);
        }
        if (m_partEdit)
            ShowWindow(m_partEdit, SW_HIDE); // only floats over a row during in-place editing
        if (m_delimEdit)
            ShowWindow(m_delimEdit, m_kind == "env" && showDetail && !raw && m_delimCustom ? SW_SHOW : SW_HIDE);
        if (!showDetail || raw) {
            if (m_partsList) ShowWindow(m_partsList, SW_HIDE);
            if (m_partEdit) ShowWindow(m_partEdit, SW_HIDE);
            for (HWND e : {m_nameEdit, m_typeEdit, m_valueEdit, m_enabledChk, m_ipEdit, m_hostEdit, m_commentEdit})
                if (e)
                    ShowWindow(e, SW_HIDE);
            return;
        }
        // Label row 1 is painted at top+2; editable controls live at top+20..42.
        // The mode/delimiter toolbar is at top+48. Items start at top+76;
        // raw values retain their label at top+74 and begin at top+90.
        const int lx = m_rcDetail.left + 4;
        const int top = m_rcDetail.top + 20;
        const int valTop = m_rcDetail.top + (m_kind == "env" && m_listMode ? 76 : 90);
        const int rowW = m_rcDetail.right - lx - 4 - 200; // reserve the action buttons
        const int valueW = m_rcDetail.right - lx - 4;
        const int valueH = std::max(60, static_cast<int>(m_rcDetail.bottom - valTop - 6));
        if (m_kind == "env") {
            show(m_nameEdit, lx, top, rowW / 2 - 6, 22);
            show(m_typeEdit, lx + rowW / 2 + 4, top, rowW / 2 - 6, 22);
            if (m_delimEdit) {
                if (m_delimCustom && m_listMode && rowEditable(m_sel)) {
                    ShowWindow(m_delimEdit, SW_SHOW);
                    MoveWindow(m_delimEdit, m_rcDelimiter.left, m_rcDelimiter.top,
                               m_rcDelimiter.right - m_rcDelimiter.left,
                               m_rcDelimiter.bottom - m_rcDelimiter.top, TRUE);
                } else {
                    m_delimCustom = false;
                    ShowWindow(m_delimEdit, SW_HIDE);
                }
            }
            if (m_listMode) {
                if (m_valueEdit)
                    ShowWindow(m_valueEdit, SW_HIDE);
                // v0.6.0 item table: the list is a full-width owner-drawn table
                // with its own column header (# / item / chars / action), as in
                // the design. There is no side strip any more: the per-row ×
                // lives in the last column; Up/Down buttons live in the footer.
                constexpr int kPartHeadH = 20;
                m_rcPartHeader = {lx, valTop, lx + valueW, valTop + kPartHeadH};
                const int listTop = valTop + kPartHeadH;
                const int listH = std::max(24, valueH - kPartHeadH);
                show(m_partsList, lx, listTop, valueW, listH);
                EnableWindow(m_partsList, rowEditable(m_sel) ? TRUE : FALSE);
                SetRectEmpty(&m_rcPartActions);
                SetRectEmpty(&m_rcPartInsert);
                SetRectEmpty(&m_rcPartDelete);
                if (m_partInline >= 0 && m_partInline < static_cast<int>(m_valueParts.size())) {
                    RECT ir {};
                    // LB_GETITEMRECT returns LB_ERR (-1) on failure; modern
                    // list controls return 1 (not LB_OKAY) for a visible item,
                    // so only the error value may be tested here. Comparing
                    // against LB_OKAY silently broke the in-place editor.
                    if (SendMessageW(m_partsList, LB_GETITEMRECT,
                                     static_cast<WPARAM>(m_partInline),
                                     reinterpret_cast<LPARAM>(&ir)) != LB_ERR) {
                        POINT pt {ir.left, ir.top};
                        MapWindowPoints(m_partsList, m_hwnd, &pt, 1);
                        show(m_partEdit, pt.x, pt.y, ir.right - ir.left, ir.bottom - ir.top);
                        SendMessageW(m_partEdit, EM_SETREADONLY, FALSE, 0);
                    }
                } else if (m_partEdit) {
                    ShowWindow(m_partEdit, SW_HIDE);
                }
            } else {
                if (m_partsList)
                    ShowWindow(m_partsList, SW_HIDE);
                if (m_partEdit)
                    ShowWindow(m_partEdit, SW_HIDE);
                m_partInline = -1;
                SetRectEmpty(&m_rcPartActions);
                SetRectEmpty(&m_rcPartHeader);
                show(m_valueEdit, lx, valTop, valueW, valueH);
                SetRectEmpty(&m_rcPartUp);
                SetRectEmpty(&m_rcPartDown);
                SetRectEmpty(&m_rcPartInsert);
                SetRectEmpty(&m_rcPartDelete);
            }
        } else {
            const int checkW = m_rcHostEnabled.right - m_rcHostEnabled.left;
            const int checkH = m_rcHostEnabled.bottom - m_rcHostEnabled.top;
            show(m_enabledChk, m_rcHostEnabled.left + 2, m_rcHostEnabled.top + 5,
                 std::min(110, checkW - 4), std::min(20, checkH - 2));
            show(m_ipEdit, m_rcHostIp.left, m_rcHostIp.top,
                 m_rcHostIp.right - m_rcHostIp.left, m_rcHostIp.bottom - m_rcHostIp.top);
            show(m_hostEdit, m_rcHostName.left, m_rcHostName.top,
                 m_rcHostName.right - m_rcHostName.left, m_rcHostName.bottom - m_rcHostName.top);
            show(m_commentEdit, m_rcHostComment.left, m_rcHostComment.top,
                 m_rcHostComment.right - m_rcHostComment.left, m_rcHostComment.bottom - m_rcHostComment.top);
        }
    }
    void show(HWND e, int x, int y, int w, int h)
    {
        if (!e)
            return;
        ShowWindow(e, SW_SHOW);
        MoveWindow(e, x, y, w, h, TRUE);
    }

    // ---- delimited-value list mode (env only) ----
    struct DelimiterPreset {
        std::string value;
        const wchar_t* zhLabel;
        const wchar_t* enLabel;
    };
    static std::vector<std::string>& delimiterPresets()
    {
        static std::vector<std::string> kDelims = {";", ",", ":", "\t", "|", " "};
        return kDelims;
    }
    static std::vector<DelimiterPreset>& delimiterPresetItems()
    {
        static std::vector<DelimiterPreset> kPresets = {
            {";", L"分号 (;)", L"Semicolon (;)"},
            {",", L"逗号 (,)", L"Comma (,)"},
            {":", L"冒号 (:)", L"Colon (:)"},
            {"\t", L"制表符 (Tab)", L"Tab"},
            {"|", L"竖线 (|)", L"Pipe (|)"},
            {" ", L"空格 (Space)", L"Space"}};
        return kPresets;
    }
    // Preset menu for the delimiter button. Picking a preset applies it at
    // once (choosing from a menu is already explicit); 自定义… swaps the button
    // for an edit box that is applied with Enter and dropped with Esc.
    static constexpr UINT kCmdDelimPreset = 200; // 200..205, then custom 206
    static constexpr UINT kCmdDelimCustom = 206;
    void showDelimiterMenu()
    {
        HMENU m = CreatePopupMenu();
        if (!m)
            return;
        const auto& ps = delimiterPresetItems();
        for (std::size_t i = 0; i < ps.size(); ++i)
            AppendMenuW(m, MF_STRING | (ps[i].value == m_listDelimiter ? MF_CHECKED : 0),
                        kCmdDelimPreset + i, m_args.zh ? ps[i].zhLabel : ps[i].enLabel);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        bool custom = true;
        for (const auto& p : ps)
            if (p.value == m_listDelimiter)
                custom = false;
        AppendMenuW(m, MF_STRING | (custom ? MF_CHECKED : 0), kCmdDelimCustom,
                    m_args.zh ? L"自定义…" : L"Custom…");
        POINT pt {m_rcDelimiter.left, m_rcDelimiter.bottom};
        ClientToScreen(m_hwnd, &pt);
        TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(m);
    }
    void onDelimiterCommand(UINT id)
    {
        const auto& ps = delimiterPresetItems();
        if (id >= kCmdDelimPreset && id < kCmdDelimPreset + ps.size()) {
            m_delimCustom = false;
            m_pendingDelim = ps[id - kCmdDelimPreset].value;
            applyDelimiter();
            return;
        }
        if (id == kCmdDelimCustom) {
            m_delimCustom = true;
            applyLayout();
            m_loading = true;
            SetWindowTextW(m_delimEdit, wideFromUtf8(m_listDelimiter == "\t" ? std::string {} : m_listDelimiter).c_str());
            m_loading = false;
            SetFocus(m_delimEdit);
            SendMessageW(m_delimEdit, EM_SETSEL, 0, -1);
            InvalidateRect(m_hwnd, nullptr, TRUE);
        }
    }
    std::string delimiterValueForText(const std::string& text) const
    {
        for (const auto& p : delimiterPresetItems()) {
            const std::string label = utf8FromWide(m_args.zh ? p.zhLabel : p.enLabel);
            if (label == text)
                return p.value;
        }
        return text;
    }
    std::wstring delimiterTextForValue(const std::string& value) const
    {
        for (const auto& p : delimiterPresetItems())
            if (p.value == value)
                return m_args.zh ? p.zhLabel : p.enLabel;
        return wideFromUtf8(value);
    }
    std::string pendingDelimiterValue() const
    {
        if (!m_pendingDelim.empty())
            return m_pendingDelim;
        if (m_delimCustom && m_delimEdit)
            return delimiterValueForText(getEdit(m_delimEdit));
        return m_listDelimiter;
    }
    bool delimiterApplyEnabled() const
    {
        const std::string next = pendingDelimiterValue();
        return m_kind == "env" && m_listMode && rowEditable(m_sel) && !next.empty() && next != m_listDelimiter;
    }
    // Leave custom mode and show the applied delimiter on the button again.
    void syncDelimiterComboText()
    {
        m_pendingDelim.clear();
        if (m_delimCustom) {
            m_delimCustom = false;
            if (m_delimEdit)
                ShowWindow(m_delimEdit, SW_HIDE);
        }
        if (m_hwnd)
            InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    std::string delimiterKey() const
    {
        if (m_kind != "env" || m_visible.empty() || m_sel < 0 || m_sel >= static_cast<int>(m_visible.size()))
            return {};
        const auto& r = m_env.rows[m_visible[m_sel]];
        return r.scope + "/" + envh::toLower(r.name);
    }
    std::string preferredDelimiter(const std::string& name, const std::string& value) const
    {
        const std::string key = delimiterKey();
        const auto it = m_delimPrefs.find(key);
        if (!key.empty() && it != m_delimPrefs.end() && !it->second.empty())
            return it->second;
        return guessDelimiter(name, value);
    }
    static bool isPathName(const std::string& name)
    {
        std::string up;
        for (char c : name)
            up += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        return up == "PATH";
    }
    static std::string guessDelimiter(const std::string& name, const std::string& value)
    {
        if (isPathName(name))
            return ";";
        // A space is a valid delimiter but a terrible auto-guess: almost any
        // real value contains one (e.g. "C:\Program Files\App"). Only pick it
        // explicitly from the menu.
        for (const std::string& d : delimiterPresets())
            if (d != " " && value.find(d) != std::string::npos)
                return d;
        return ";";
    }
    std::string nextDelimiter() const
    {
        const auto& ds = delimiterPresets();
        for (std::size_t i = 0; i < ds.size(); ++i)
            if (ds[i] == m_listDelimiter)
                return ds[(i + 1) % ds.size()];
        return ds[0];
    }
    std::string delimiterLabel() const
    {
        if (m_listDelimiter == "\t")
            return m_args.zh ? "制表符" : "TAB";
        if (m_listDelimiter == " ")
            return m_args.zh ? "空格" : "Space";
        return m_listDelimiter;
    }

    int partListVisibleRows() const
    {
        if (!m_partsList)
            return 10;
        const int itemH = static_cast<int>(SendMessageW(m_partsList, LB_GETITEMHEIGHT, 0, 0));
        RECT cr {};
        GetClientRect(m_partsList, &cr);
        const int rows = itemH > 0 ? (cr.bottom - cr.top) / itemH : 0;
        return rows > 0 ? rows : 10;
    }
    void ensurePartVisible()
    {
        if (!m_partsList || m_partSel < 0)
            return;
        const int rows = partListVisibleRows();
        const int top = static_cast<int>(SendMessageW(m_partsList, LB_GETTOPINDEX, 0, 0));
        if (m_partSel < top)
            SendMessageW(m_partsList, LB_SETTOPINDEX, m_partSel, 0);
        else if (m_partSel >= top + rows)
            SendMessageW(m_partsList, LB_SETTOPINDEX, m_partSel - rows + 1, 0);
    }
    // A Win32 list box has no "replace this item's text" message, so an in-place
    // update is delete + insert at the same index. The caret is re-asserted
    // because removing the selected item can clear it, and PartsGuard keeps the
    // resulting LBN_SELCHANGE from re-entering the commit path.
    void updatePartRow(int index)
    {
        if (!m_partsList || index < 0 || index >= static_cast<int>(m_valueParts.size()))
            return;
        const std::wstring wide = wideFromUtf8(m_valueParts[index]);
        const PartsGuard g {this};
        SendMessageW(m_partsList, LB_DELETESTRING, index, 0);
        SendMessageW(m_partsList, LB_INSERTSTRING, index, reinterpret_cast<LPARAM>(wide.c_str()));
        SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
    }
    void showCurrentPart()
    {
        const std::string text = m_partSel >= 0 && m_partSel < static_cast<int>(m_valueParts.size())
            ? m_valueParts[m_partSel]
            : std::string {};
        setEdit(m_partEdit, text, !rowEditable(m_sel));
    }
    // The "当前项 N/M" counter and the Up/Down enable states are painted in the
    // WM_PAINT buffer, not by the list box child. List-only updates (select,
    // move, insert, delete) must therefore invalidate that strip or the counter
    // stays on the previous item.
    void invalidatePartChrome()
    {
        RECT rc = m_rcItemHint;
        UnionRect(&rc, &rc, &m_rcPartUp);
        UnionRect(&rc, &rc, &m_rcPartDown);
        UnionRect(&rc, &rc, &m_rcPartInsert);
        UnionRect(&rc, &rc, &m_rcPartDelete);
        UnionRect(&rc, &rc, &m_rcPartActions);
        InvalidateRect(m_hwnd, &rc, FALSE);
    }

    // ---- v0.6.0 full-width item list: per-row markers and in-place editing --
    bool partChanged(int i) const
    {
        if (i < 0 || i >= static_cast<int>(m_valueParts.size()) || m_partBase.empty())
            return false;
        const std::string& v = m_valueParts[static_cast<std::size_t>(i)];
        // A value that never appeared in the row as opened is new or edited.
        // Reordered / shifted items are deliberately NOT flagged: deleting one
        // item moves every later index, and painting them all red would be
        // noise instead of information.
        return std::find(m_partBase.begin(), m_partBase.end(), v) == m_partBase.end();
    }
    bool partStaged(int i) const
    {
        return i >= 0 && i < static_cast<int>(m_partDeleted.size())
            && m_partDeleted[static_cast<std::size_t>(i)];
    }
    int stagedPartCount() const
    {
        int n = 0;
        for (char f : m_partDeleted)
            n += f != 0;
        return n;
    }
    // ---- v0.7.1 item-list multi-select -------------------------------------
    bool partSelected(int i) const
    {
        return i >= 0 && i < static_cast<int>(m_partMulti.size())
            && m_partMulti[static_cast<std::size_t>(i)] != 0;
    }
    int partSelectedCount() const
    {
        int n = 0;
        for (char f : m_partMulti)
            n += f != 0;
        return n;
    }
    // Rows the next item command acts on: the visual selection, or the caret
    // row when nothing is multi-selected (single-select behaviour preserved).
    std::vector<int> partTargetRows() const
    {
        std::vector<int> out;
        if (partSelectedCount() > 0) {
            for (int i = 0; i < static_cast<int>(m_partMulti.size()); ++i)
                if (m_partMulti[static_cast<std::size_t>(i)])
                    out.push_back(i);
        } else if (m_partSel >= 0 && m_partSel < static_cast<int>(m_valueParts.size())) {
            out.push_back(m_partSel);
        }
        return out;
    }
    void partSelectionClear()
    {
        std::fill(m_partMulti.begin(), m_partMulti.end(), 0);
    }
    void partSelectionSetAll(bool on)
    {
        m_partMulti.assign(m_valueParts.size(), on ? 1 : 0);
        if (on && m_partMulti.size() > static_cast<std::size_t>(std::max(0, m_partSel)))
            m_partMulti[static_cast<std::size_t>(m_partSel)] = 1;
    }
    // Apply a click gesture on row idx. Returns true when the click was
    // consumed (additive or range); a plain click falls through to the
    // normal single-select caret move.
    bool partSelectionGesture(int idx, bool additive, bool range)
    {
        if (idx < 0 || idx >= static_cast<int>(m_valueParts.size()))
            return false;
        if (m_partMulti.size() != m_valueParts.size())
            m_partMulti.assign(m_valueParts.size(), 0);
        if (range) {
            const int a = std::max(0, std::min(m_partAnchor, idx));
            const int b = std::min(static_cast<int>(m_valueParts.size()) - 1,
                                   std::max(m_partAnchor, idx));
            partSelectionClear();
            for (int i = a; i <= b; ++i)
                m_partMulti[static_cast<std::size_t>(i)] = 1;
            m_partSel = idx;
        } else if (additive) {
            m_partMulti[static_cast<std::size_t>(idx)] =
                m_partMulti[static_cast<std::size_t>(idx)] ? 0 : 1;
            m_partAnchor = idx;
            m_partSel = idx;
        } else {
            return false;
        }
        if (m_partsList)
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
        invalidatePartChrome();
        InvalidateRect(m_partsList, nullptr, FALSE);
        return true;
    }
    PartState partState() const
    {
        return {m_valueParts, m_partBase, m_partNew, m_partDeleted, m_partMulti,
                m_listDelimiter, m_partSel, m_partAnchor};
    }
    void restorePartState(const PartState& s)
    {
        m_valueParts = s.values;
        m_partBase = s.base;
        m_partNew = s.added;
        m_partDeleted = s.deleted;
        m_partMulti = s.selected;
        m_listDelimiter = s.delimiter;
        m_partSel = s.caret;
        m_partAnchor = s.anchor;
        m_partInline = -1;
        m_partInlineOriginal.clear();
    }
    static std::string partValue(const PartState& s)
    {
        std::vector<std::string> live;
        for (std::size_t i = 0; i < s.values.size(); ++i)
            if (i >= s.deleted.size() || !s.deleted[i])
                live.push_back(s.values[i]);
        return envh::edit::joinDelimited(live, s.delimiter);
    }
    static bool samePartContent(const PartState& a, const PartState& b)
    {
        return a.values == b.values && a.added == b.added && a.deleted == b.deleted
            && a.delimiter == b.delimiter;
    }
    static bool partPending(const PartSession& s)
    {
        return partValue(s.current) != s.sourceValue
            || std::any_of(s.current.deleted.begin(), s.current.deleted.end(), [](char c) { return c != 0; });
    }
    bool envRowPending(std::size_t i) const
    {
        if (i >= m_env.rows.size() || m_env.rows[i].deleted)
            return false;
        const auto& row = m_env.rows[i];
        const auto it = m_partSessions.find(row.scope + "/" + row.name);
        return it != m_partSessions.end() && it->second.sourceValue == row.value && partPending(it->second);
    }
    const wchar_t* envStateLabel(envh::edit::RowState state, bool pending) const
    {
        const bool zh = m_args.zh;
        switch (state) {
        case envh::edit::RowState::Added:
            return pending ? (zh ? L"待提交 · 新增未保存" : L"Pending · New/unsaved")
                           : (zh ? L"新增 · 未保存" : L"New · Unsaved");
        case envh::edit::RowState::Modified:
            return pending ? (zh ? L"待提交 · 未保存" : L"Pending · Unsaved")
                           : (zh ? L"未保存" : L"Unsaved");
        case envh::edit::RowState::Deleted:
            return zh ? L"删除 · 未保存" : L"Deleted · Unsaved";
        default:
            return pending ? (zh ? L"待提交" : L"Pending") : L"";
        }
    }
    bool pendingPartChanges() const
    {
        for (const auto& item : m_partSessions)
            if (partPending(item.second))
                return true;
        return false;
    }
    void stashPartView()
    {
        auto it = m_partSessions.find(m_partRef);
        if (m_listMode && it != m_partSessions.end())
            it->second.current = partState();
    }
    void rememberPartChange()
    {
        if (m_partRef.empty() || m_loading)
            return;
        auto it = m_partSessions.find(m_partRef);
        const PartState next = partState();
        if (it == m_partSessions.end()) {
            if (samePartContent(next, m_partInitial))
                return;
            it = m_partSessions.emplace(m_partRef,
                PartSession {m_partSourceValue, m_partInitial, {}, {}}).first;
        }
        auto& session = it->second;
        if (!samePartContent(next, session.current)) {
            session.undo.push_back(session.current);
            if (session.undo.size() > 100)
                session.undo.erase(session.undo.begin());
            session.redo.clear();
        }
        session.current = next;
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    bool undoPartChange(bool redo)
    {
        const HWND focus = GetFocus();
        auto it = m_partSessions.find(currentRowRef());
        if (it == m_partSessions.end())
            return false;
        auto& s = it->second;
        auto& from = redo ? s.redo : s.undo;
        auto& to = redo ? s.undo : s.redo;
        if (from.empty())
            return false;
        to.push_back(s.current);
        s.current = from.back();
        from.pop_back();
        restorePartState(s.current);
        syncRawValueEdit();
        rebuildPartsList(true);
        applyLayout();
        if (focus == m_partsList || focus == m_partEdit)
            SetFocus(m_partsList);
        InvalidateRect(m_hwnd, nullptr, TRUE);
        return true;
    }
    void clearPartSessions()
    {
        m_partSessions.clear();
        m_partRef.clear();
        m_partInline = -1;
    }
    winshun::Json envDraft()
    {
        stashPartView();
        auto draft = m_env.toDraft().toObject();
        winshun::Json::Array items;
        for (const auto& entry : m_partSessions) {
            const auto& s = entry.second;
            if (!partPending(s))
                continue;
            winshun::Json::Object item;
            item["ref"] = entry.first;
            item["source"] = s.sourceValue;
            item["delimiter"] = s.current.delimiter;
            winshun::Json::Array values, base, added, deleted;
            for (const auto& v : s.current.values) values.emplace_back(v);
            for (const auto& v : s.current.base) base.emplace_back(v);
            for (char v : s.current.added) added.emplace_back(v != 0);
            for (char v : s.current.deleted) deleted.emplace_back(v != 0);
            item["values"] = winshun::Json(std::move(values));
            item["base"] = winshun::Json(std::move(base));
            item["added"] = winshun::Json(std::move(added));
            item["deleted"] = winshun::Json(std::move(deleted));
            items.emplace_back(std::move(item));
        }
        draft["itemCandidates"] = winshun::Json(std::move(items));
        return winshun::Json(std::move(draft));
    }
    void applyPartSessions(bool all)
    {
        stashPartView();
        bool pushed = false;
        for (auto it = m_partSessions.begin(); it != m_partSessions.end();) {
            if (!all && it->first != currentRowRef()) { ++it; continue; }
            const auto& session = it->second;
            for (auto& row : m_env.rows) {
                if (row.scope + "/" + row.name != it->first || row.deleted
                    || row.value != session.sourceValue)
                    continue;
                const std::string value = partValue(session.current);
                if (value != row.value) {
                    if (!pushed) { endEdit(); m_env.pushUndo(); pushed = true; }
                    row.value = value;
                }
                break;
            }
            it = m_partSessions.erase(it);
        }
        m_partRef.clear(); // refresh must not resurrect the committed candidate
        refreshDetail();
        applyLayout();
    }
    // A preview value only: navigation never writes it into the model.
    std::string joinedPartsValue() const
    {
        if (!m_listMode)
            return {};
        if (stagedPartCount() == 0)
            return envh::edit::joinDelimited(m_valueParts, m_listDelimiter);
        std::vector<std::string> live;
        live.reserve(m_valueParts.size());
        for (std::size_t i = 0; i < m_valueParts.size(); ++i)
            if (!(i < m_partDeleted.size() && m_partDeleted[i]))
                live.push_back(m_valueParts[i]);
        return envh::edit::joinDelimited(live, m_listDelimiter);
    }
    // The 提交修改 button: apply staged item operations into the row value
    // (still model-staged; the registry write remains behind Ctrl+S).
    void commitPartChanges()
    {
        if (m_kind != "env" || !rowEditable(m_sel))
            return;
        commitPartEdit();
        applyPartSessions(false);
        if (m_listMode)
            SetFocus(m_partsList);
        invalidatePartChrome();
        InvalidateRect(m_hwnd, nullptr, TRUE);
        setStatus(m_args.zh ? "已提交分隔项修改；按 Ctrl+S 写入注册表"
                            : "Item changes committed; press Ctrl+S to write",
                  0);
    }

    // ---- v0.6.0 item table: owner-drawn columns ----------------------------
    // Design columns: # | item | chars | action. The list is an
    // LBS_OWNERDRAWFIXED list box so a row can carry a number, a per-row
    // character count, a state tag and a × while keeping native scrolling,
    // selection and the in-place edit overlay.
    static constexpr int kNumW = 46;   // # column
    static constexpr int kCharsW = 76; // per-row character count
    static constexpr int kActW = 84;   // state tag / ×

    void partColumns(const RECT& row, int& numR, int& itemR, int& charsR, int& actR) const
    {
        numR = row.left + kNumW;
        actR = row.right - kActW;
        charsR = actR - kCharsW;
        itemR = numR;
    }

    void drawPartHeader(HDC dc, HFONT small, COLORREF fg, COLORREF dim)
    {
        if (m_rcPartHeader.right <= m_rcPartHeader.left)
            return;
        HBRUSH hb = CreateSolidBrush(th().kSubHeadBg);
        FillRect(dc, &m_rcPartHeader, hb);
        DeleteObject(hb);
        HGDIOBJ of = SelectObject(dc, small);
        SetTextColor(dc, dim);
        int numR = 0, itemR = 0, charsR = 0, actR = 0;
        partColumns(m_rcPartHeader, numR, itemR, charsR, actR);
        const wchar_t* labels[4] = {L"#", m_args.zh ? L"分隔项" : L"Item",
                                    m_args.zh ? L"字符" : L"Chars",
                                    m_args.zh ? L"操作" : L"Action"};
        const int xs[4] = {m_rcPartHeader.left, itemR, charsR, actR};
        for (int c = 0; c < 4; ++c) {
            RECT r {xs[c] + (c == 0 ? 10 : 4), m_rcPartHeader.top,
                    (c == 3 ? m_rcPartHeader.right - 6 : xs[c + 1 > 3 ? 3 : c + 1] - 4),
                    m_rcPartHeader.bottom};
            DrawTextW(dc, labels[c], -1, &r,
                      (c == 2 || c == 3 ? DT_RIGHT : DT_LEFT)
                          | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, of);
        HPEN p = CreatePen(PS_SOLID, 1, th().kBorder);
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, m_rcPartHeader.left, m_rcPartHeader.bottom - 1, nullptr);
        LineTo(dc, m_rcPartHeader.right, m_rcPartHeader.bottom - 1);
        SelectObject(dc, op);
        DeleteObject(p);
        (void)fg;
    }

    void drawPartRow(const DRAWITEMSTRUCT& di, HFONT small, HFONT ui, COLORREF fg, COLORREF dim)
    {
        const int i = static_cast<int>(di.itemID);
        if (i < 0 || i >= static_cast<int>(m_valueParts.size()))
            return;
        HDC dc = di.hDC;
        RECT row = di.rcItem;
        const bool sel = (di.itemState & ODS_SELECTED) != 0;
        const bool multi = partSelected(i); // v0.7.1 multi-select highlight
        const bool changed = partChanged(i);
        const bool added = i < static_cast<int>(m_partNew.size()) && m_partNew[static_cast<std::size_t>(i)];
        const bool staged = partStaged(i);
        const COLORREF selBg = th().kPartSelBg;
        const COLORREF addBg = th().kPartAddBg;
        const COLORREF red = th().kDeletedText;
        const COLORREF blue = th().kAddedText;
        HBRUSH bg = CreateSolidBrush(staged ? th().kDirtyBg
                          : changed || added ? addBg
                          : (multi || sel) ? selBg
                                : (i % 2 ? th().kStripe : th().kBg));
        FillRect(dc, &row, bg);
        DeleteObject(bg);
        if ((multi && !sel) || sel || changed || added || staged) {
            RECT bar {row.left, row.top, row.left + 3, row.bottom};
            HBRUSH bb = CreateSolidBrush(multi && !sel ? (th().kAccent)
                                             : sel ? (th().kSelBar)
                                                   : (th().kDangerSoft));
            FillRect(dc, &bar, bb);
            DeleteObject(bb);
        }
        int numR = 0, itemR = 0, charsR = 0, actR = 0;
        partColumns(row, numR, itemR, charsR, actR);
        HGDIOBJ of = SelectObject(dc, small);
        SetTextColor(dc, dim);
        wchar_t num[8] = {};
        swprintf_s(num, L"%02d", i + 1);
        RECT nr {row.left + 10, row.top, numR - 4, row.bottom};
        DrawTextW(dc, num, -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // Per-row character count, right-aligned in its column.
        const std::size_t used = envh::utf16Units(m_valueParts[static_cast<std::size_t>(i)]);
        RECT cr {charsR + 2, row.top, actR - 10, row.bottom};
        SetTextColor(dc, staged || changed || added ? red : dim);
        DrawTextW(dc, std::to_wstring(used).c_str(), -1, &cr,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        of = SelectObject(dc, ui);
        RECT ir {itemR, row.top, charsR - 6, row.bottom};
        const bool hasSpace = m_listDelimiter == " "
            && m_valueParts[static_cast<std::size_t>(i)].find(' ') != std::string::npos;
        SetTextColor(dc, staged || changed || added ? red : (sel ? blue : fg));
        DrawTextW(dc, wideFromUtf8(m_valueParts[static_cast<std::size_t>(i)]).c_str(), -1, &ir,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (staged) {
            // GDI has no strikethrough text flag here; draw the line by hand
            // through the vertical centre of the text.
            const std::wstring wide = wideFromUtf8(m_valueParts[static_cast<std::size_t>(i)]);
            SIZE ext {};
            GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &ext);
            TEXTMETRICW tm {};
            GetTextMetricsW(dc, &tm);
            const int mid = ir.top + (ir.bottom - ir.top) / 2 + tm.tmDescent / 2 - 1;
            const int w = std::min<int>(ir.right - ir.left - 4, ext.cx);
            HPEN sp = CreatePen(PS_SOLID, 1, red);
            HGDIOBJ osp = SelectObject(dc, sp);
            MoveToEx(dc, ir.left, mid, nullptr);
            LineTo(dc, ir.left + w, mid);
            SelectObject(dc, osp);
            DeleteObject(sp);
        }
        SelectObject(dc, of);
        of = SelectObject(dc, small);
        RECT ar {actR, row.top, row.right - 10, row.bottom};
        if (staged) {
            SetTextColor(dc, red);
            DrawTextW(dc, m_args.zh ? L"− 待删除" : L"− staged", -1, &ar,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (added) {
            SetTextColor(dc, red);
            DrawTextW(dc, m_args.zh ? L"+ 新增" : L"+ new", -1, &ar,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (changed) {
            SetTextColor(dc, red);
            DrawTextW(dc, m_args.zh ? L"● 已改" : L"● edited", -1, &ar,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            SetTextColor(dc, hasSpace ? (th().kWarnText) : dim);
            DrawTextW(dc, hasSpace ? L"· ×" : L"×", -1, &ar,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, of);
    }

    // Click in the action cell of row `idx`? Returns true when the caller should
    // delete that item (only rows without a state tag carry a ×).
    bool partActionHit(int x) const
    {
        if (!m_partsList)
            return false;
        RECT lr {};
        GetClientRect(m_partsList, &lr);
        return x >= lr.right - kActW && x <= lr.right;
    }

    void beginPartInline(int idx)
    {
        if (!rowEditable(m_sel) || idx < 0 || idx >= static_cast<int>(m_valueParts.size()))
            return;
        if (partStaged(idx)) // a staged-deleted item is read-only until restored
            return;
        m_partSel = idx;
        const PartsGuard g {this}; // LB_SETCURSEL must not re-enter the commit path
        if (m_partsList)
            SendMessageW(m_partsList, LB_SETCURSEL, static_cast<WPARAM>(idx), 0);
        m_partInline = idx;
        m_partInlineOriginal = m_valueParts[static_cast<std::size_t>(idx)];
        m_loading = true;
        setEdit(m_partEdit, m_partInlineOriginal, false);
        m_loading = false;
        applyLayout();
        if (m_partEdit) {
            SetFocus(m_partEdit);
            SendMessageW(m_partEdit, EM_SETSEL, 0, -1);
        }
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void endPartInline(bool commit)
    {
        const int idx = m_partInline;
        if (idx < 0)
            return;
        if (!commit && idx < static_cast<int>(m_valueParts.size())) {
            m_valueParts[static_cast<std::size_t>(idx)] = m_partInlineOriginal;
            updatePartRow(idx);
            syncRawValueEdit();
            rememberPartChange();
        }
        m_partInline = -1;
        m_partInlineOriginal.clear();
        applyLayout();
        if (m_partsList)
            SetFocus(m_partsList);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    // A new model value/delimiter invalidates every index in the old view.
    void resetPartState()
    {
        m_partNew.assign(m_valueParts.size(), 0);
        m_partDeleted.assign(m_valueParts.size(), 0);
        m_partMulti.assign(m_valueParts.size(), 0);
        m_partAnchor = m_partSel;
        m_partInline = -1;
        m_partInlineOriginal.clear();
    }
    // Rendering must not mutate the value or its metadata. It is also used
    // after removing new items from a mixed selection, with other items staged.
    void rebuildPartsList(bool preserveTop = false)
    {
        if (!m_partsList)
            return;
        const PartsGuard g {this};
        const int oldTop = preserveTop
            ? std::max(0, static_cast<int>(SendMessageW(m_partsList, LB_GETTOPINDEX, 0, 0)))
            : 0;
        SendMessageW(m_partsList, LB_RESETCONTENT, 0, 0);
        for (const auto& part : m_valueParts) {
            const std::wstring wide = wideFromUtf8(part);
            SendMessageW(m_partsList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        }
        if (!m_valueParts.empty()) {
            m_partSel = std::max(0, std::min(m_partSel, static_cast<int>(m_valueParts.size()) - 1));
            const int rows = partListVisibleRows();
            const int maxTop = std::max(0, static_cast<int>(m_valueParts.size()) - rows);
            SendMessageW(m_partsList, LB_SETTOPINDEX, std::min(oldTop, maxTop), 0);
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        } else {
            m_partSel = 0;
        }
        showCurrentPart();
        if (m_listMode && m_partEdit)
            SendMessageW(m_partEdit, EM_SETREADONLY, rowEditable(m_sel) ? FALSE : TRUE, 0);
    }
    void commitPartEdit()
    {
        if (!m_listMode || m_loading || m_valueParts.empty() || !rowEditable(m_sel))
            return;
        if (partStaged(m_partSel)) // staged-deleted items stay read-only
            return;
        const std::string next = getEdit(m_partEdit);
        if (m_partSel < 0 || m_partSel >= static_cast<int>(m_valueParts.size())
            || m_valueParts[m_partSel] == next)
            return;
        m_valueParts[m_partSel] = next;
        updatePartRow(m_partSel);
        syncRawValueEdit();
        rememberPartChange();
    }
    // Keep the (hidden) raw value box in sync with the joined list, so toggling
    // back with Ctrl+R and any control-text read see the current value.
    void syncRawValueEdit()
    {
        m_loading = true;
        setEdit(m_valueEdit, joinedPartsValue(), !rowEditable(m_sel));
        m_loading = false;
    }
    void addPart()
    {
        if (!rowEditable(m_sel))
            return;
        const PartsGuard g {this};
        commitPartEdit();
        const int insertAt = std::max(0, std::min(m_partSel + 1, static_cast<int>(m_valueParts.size())));
        m_valueParts.insert(m_valueParts.begin() + insertAt, std::string {});
        m_partNew.insert(m_partNew.begin() + insertAt, 1);
        if (insertAt <= static_cast<int>(m_partDeleted.size()))
            m_partDeleted.insert(m_partDeleted.begin() + insertAt, 0);
        m_partSel = insertAt;
        m_partMulti.assign(m_valueParts.size(), 0);
        m_partAnchor = m_partSel;
        if (m_partsList) {
            const std::wstring empty;
            SendMessageW(m_partsList, LB_INSERTSTRING, insertAt, reinterpret_cast<LPARAM>(empty.c_str()));
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        }
        invalidatePartChrome();
        syncRawValueEdit();
        rememberPartChange();
        beginPartInline(m_partSel);
    }
    void deletePart()
    {
        if (!rowEditable(m_sel) || m_valueParts.empty()
            || m_partSel < 0 || m_partSel >= static_cast<int>(m_valueParts.size()))
            return;
        const PartsGuard g {this};
        commitPartEdit();
        // v0.7.1 multi-select: stage every selected row. Without a visual
        // selection the caret row is the only target (single-select).
        const std::vector<int> targets = partTargetRows();
        if (targets.empty())
            return;
        // v0.7.0 two-stage delete: x toggles the STAGED state. Staged items
        // stay in the list (red, struck through, "− 待删除") and are excluded
        // from the joined value; nothing is erased until 提交修改 / Ctrl+S.
        // An ADDED item never existed in the value, so it is erased at once.
        if (m_partDeleted.size() < m_valueParts.size())
            m_partDeleted.resize(m_valueParts.size(), 0);
        int addedErased = 0;
        int stagedNow = 0, restored = 0;
        for (const int row : targets) {
            const bool addedRow = row < static_cast<int>(m_partNew.size())
                && m_partNew[static_cast<std::size_t>(row)];
            if (addedRow) {
                m_partDeleted[static_cast<std::size_t>(row)] = 2; // marker: erase below
                ++addedErased;
            } else {
                const char next = m_partDeleted[static_cast<std::size_t>(row)] ? 0 : 1;
                m_partDeleted[static_cast<std::size_t>(row)] = next;
                if (next)
                    ++stagedNow;
                else
                    ++restored;
            }
        }
        // Erase the added rows (highest index first so the markers stay valid).
        for (int row = static_cast<int>(m_valueParts.size()) - 1; row >= 0; --row) {
            if (row < static_cast<int>(m_partDeleted.size())
                && m_partDeleted[static_cast<std::size_t>(row)] == 2) {
                m_valueParts.erase(m_valueParts.begin() + row);
                if (row < static_cast<int>(m_partNew.size()))
                    m_partNew.erase(m_partNew.begin() + row);
                m_partDeleted.erase(m_partDeleted.begin() + row);
            }
        }
        if (m_valueParts.empty()) {
            m_valueParts.emplace_back();
            m_partNew.assign(1, 1);
            m_partDeleted.assign(1, 0);
        }
        m_partSel = std::max(0, std::min(m_partSel, static_cast<int>(m_valueParts.size()) - 1));
        m_partMulti.assign(m_valueParts.size(), 0);
        m_partAnchor = m_partSel;
        // Existing items remain staged even when new items are erased in the
        // same batch. Rebuild only when the number of visible rows changed.
        if (addedErased > 0)
            rebuildPartsList(true);
        InvalidateRect(m_partsList, nullptr, FALSE);
        syncRawValueEdit();
        rememberPartChange();
        invalidatePartChrome();
        showCurrentPart();
        if (addedErased > 0)
            setStatus(m_args.zh ? "已移除 " + std::to_string(addedErased) + " 个新增项"
                                : std::to_string(addedErased) + " added item(s) removed",
                      0);
        else if (stagedNow > 0)
            setStatus(m_args.zh ? "分隔项已标记删除；点 提交修改 或 Ctrl+S 生效"
                                : "Item(s) staged for deletion; Commit or Ctrl+S applies it",
                      0);
        else
            setStatus(m_args.zh ? "分隔项已恢复" : "Item(s) restored", 0);
        (void)restored;
    }
    // v0.7.1: Ctrl+J / Ctrl+K move the CARET (selection highlight) up/down
    // without reordering anything; Alt+J / Alt+K / Alt+Up / Alt+Down move the
    // ITEM itself (the old Alt+Up/Down behaviour). Both close a running
    // in-place edit first (Ctrl+N drops the caret into the floating edit box,
    // and j/k must still work right after adding an item).
    void movePartCaret(int delta)
    {
        if (!m_listMode || m_valueParts.empty())
            return;
        const bool focusNear = GetFocus() == m_partsList || GetFocus() == m_partEdit;
        if (!focusNear)
            return;
        if (m_partInline >= 0)
            endPartInline(true); // commits the text and refocuses the list
        const int next = std::clamp(m_partSel + delta, 0, static_cast<int>(m_valueParts.size()) - 1);
        const PartsGuard g {this};
        commitPartEdit();
        m_partSel = next;
        if (m_partsList) {
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        }
        // Plain caret moves reset any multi-selection made before (same
        // semantic as the plain click).
        partSelectionClear();
        if (m_partMulti.size() == m_valueParts.size())
            m_partMulti[static_cast<std::size_t>(m_partSel)] = 1;
        m_partAnchor = m_partSel;
        invalidatePartChrome();
        InvalidateRect(m_partsList, nullptr, FALSE);
        showCurrentPart();
    }
    void movePart(int delta)
    {
        if (m_partInline >= 0)
            endPartInline(true);
        const int from = m_partSel;
        const int to = from + delta;
        if (!rowEditable(m_sel) || to < 0 || to >= static_cast<int>(m_valueParts.size()))
            return;
        const PartsGuard g {this};
        commitPartEdit();
        std::swap(m_valueParts[from], m_valueParts[to]);
        std::swap(m_partNew[from], m_partNew[to]);
        std::swap(m_partDeleted[from], m_partDeleted[to]);
        m_partSel = to;
        m_partMulti.assign(m_valueParts.size(), 0);
        m_partMulti[static_cast<std::size_t>(to)] = 1;
        m_partAnchor = to;
        updatePartRow(from);
        updatePartRow(to);
        if (m_partsList) {
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        }
        invalidatePartChrome();
        syncRawValueEdit();
        rememberPartChange();
        showCurrentPart();
        SetFocus(m_partsList);
    }
    // Raw <-> delimited-item editing (Ctrl+R or the mode segment).
    void setListMode(bool enabled)
    {
        if (m_kind != "env" || m_tab == 2 || m_listMode == enabled)
            return;
        commitPartEdit();
        stashPartView();
        m_listMode = enabled;
        refreshDetail();
        computeLayout();
        applyLayout();
        SetFocus(enabled ? m_partsList : m_valueEdit);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    void toggleListMode() { setListMode(!m_listMode); }

    // A space delimiter is dangerous for PATH (it would split "C:\Program
    // Files\..." into two items) and it normalises whitespace on join. Warn
    // before applying, with "No" as the default answer.
    bool confirmSpaceDelimiter()
    {
        const std::string name = (!m_visible.empty() && m_sel >= 0
                                  && m_sel < static_cast<int>(m_visible.size()))
                                     ? m_env.rows[m_visible[m_sel]].name
                                     : std::string {};
        const bool path = isPathName(name);
        const std::string body = path
            ? (m_args.zh
                ? "PATH 中常见 \"C:\\Program Files\\...\" 这类路径。按空格分隔会把一条路径切成两半，保存后可能直接毁掉 PATH。\n\n强烈建议改回分号 (;)。\n\n仍要使用空格吗？（默认：否）"
                : "PATH commonly contains entries such as \"C:\\Program Files\\...\". Splitting on spaces cuts one entry in half and can destroy PATH on save.\n\nSemicolon (;) is strongly recommended.\n\nUse a space anyway? (default: No)")
            : (m_args.zh
                ? "空格分隔会把连续空白合并为一个分隔符，写回时用单个空格，原始空白会被归一化。\n\n含空格的项会在行尾显示琥珀标记。\n\n继续？（默认：否）"
                : "A space delimiter collapses whitespace runs and rejoins with one space, normalising the original text.\n\nItems containing a space get an amber marker.\n\nContinue? (default: No)");
        ModalGuard modal {this};
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"空格分隔符" : L"Space delimiter";
        ds.subtitle = m_args.zh ? L"可能把一条路径切成两半" : L"May cut one path in half";
        ds.body = wideFromUtf8(body);
        ds.icon = envh::dlg::kWarn;
        ds.buttons = {
            {m_args.zh ? L"改用分号" : L"Use semicolon", IDNO, envh::dlg::kPlain},
            {m_args.zh ? L"仍要使用空格" : L"Use spaces", IDYES, envh::dlg::kPrimary},
        };
        ds.defButton = 0;
        return envh::dlg::show(m_hwnd, ds, th(), m_dark) == IDYES;
    }

    void applyDelimiter()
    {
        if (stagedPartCount() > 0) {
            m_pendingDelim.clear();
            syncDelimiterComboText();
            setStatus(m_args.zh ? "请先提交或撤销待删除项，再更换分隔符"
                                : "Commit or undo pending deletions before changing the delimiter", 1);
            return;
        }
        const std::string next = pendingDelimiterValue();
        if (next.empty()) {
            setStatus(m_args.zh ? "分隔符不能为空" : "Delimiter cannot be empty", 2);
            syncDelimiterComboText();
            return;
        }
        if (next == m_listDelimiter) {
            syncDelimiterComboText();
            return;
        }
        if (next == " " && !confirmSpaceDelimiter()) {
            m_pendingDelim.clear();
            syncDelimiterComboText();
            InvalidateRect(m_hwnd, nullptr, TRUE);
            return;
        }
        commitPartEdit();
        // Re-split from the current full value. If the item box has pending text,
        // commitPartEdit has already written it into this joined value.
        const std::string joined = m_listMode ? joinedPartsValue()
                                              : getEdit(m_valueEdit);
        const std::string key = delimiterKey();
        if (!key.empty())
            m_delimPrefs[key] = next;
        m_listDelimiter = next;
        if (m_listMode) {
            m_valueParts = envh::edit::splitDelimited(joined, m_listDelimiter, true);
            // A new delimiter changes what an "item" is, so the old snapshot
            // is no longer comparable: start from the re-split value.
            m_partBase = m_valueParts;
            m_partSel = 0;
            resetPartState();
            syncRawValueEdit();
            rebuildPartsList(false);
            syncDelimiterComboText();
            applyLayout();
            InvalidateRect(m_hwnd, nullptr, TRUE);
        }
        rememberPartChange();
        if (m_delimCustom)
            syncDelimiterComboText();
        m_pendingDelim.clear();
        SetFocus(m_hwnd);
        setStatus(m_args.zh ? "分隔符: " + delimiterLabel() : "delimiter: " + delimiterLabel(), 0);
    }

    // Fill the detail controls from the selected row (no model write).
    void refreshDetail()
    {
        stashPartView();
        m_loading = true;
        if (m_partInline >= 0) {
            // A different row (or tab) invalidates the floating edit box.
            m_partInline = -1;
            m_partInlineOriginal.clear();
        }
        if (m_kind == "env") {
            std::string name, type, value;
            bool ro = false, deleted = false;
            if (!m_visible.empty()) {
                const auto& r = m_env.rows[m_visible[m_sel]];
                name = r.name;
                type = r.type;
                value = r.value;
                deleted = r.deleted;
                ro = (r.scope == "system" && !m_args.systemWritable) || deleted;
            }
            setEdit(m_nameEdit, name, ro);
            setEdit(m_typeEdit, type, ro);
            setEdit(m_valueEdit, value, ro);
            m_partRef = currentRowRef();
            m_partSourceValue = value;
            m_listDelimiter = preferredDelimiter(name, value);
            m_valueParts = deleted || m_partRef.empty() ? std::vector<std::string> {}
                : envh::edit::splitDelimited(value, m_listDelimiter, true);
            m_partBase = m_valueParts;
            m_partSel = 0;
            resetPartState();
            m_partInitial = partState();
            auto candidate = m_partSessions.find(m_partRef);
            if (candidate != m_partSessions.end()) {
                if (candidate->second.sourceValue != value) {
                    m_partSessions.erase(candidate); // explicit model undo/reload/replacement
                } else if (!deleted) {
                    restorePartState(candidate->second.current);
                    setEdit(m_valueEdit, partValue(candidate->second.current), ro);
                }
            }
            if (m_listMode) {
                rebuildPartsList(false);
                syncDelimiterComboText();
                if (m_partsList)
                    EnableWindow(m_partsList, (deleted || ro) ? FALSE : TRUE);
                if (m_partEdit)
                    SendMessageW(m_partEdit, EM_SETREADONLY, (deleted || ro) ? TRUE : FALSE, 0);
            }
        } else {
            bool enabled = false, ro = false;
            std::string ip, host, comment;
            if (!m_visible.empty()) {
                const auto& r = m_hosts.rows[m_visible[m_sel]];
                ip = r.ip;
                host = join(r.hostnames);
                comment = r.comment;
                enabled = !r.commented;
                ro = !m_args.writable;
            }
            if (m_enabledChk) {
                SendMessageW(m_enabledChk, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                EnableWindow(m_enabledChk, rowEditable(m_sel) ? TRUE : FALSE);
            }
            setEdit(m_ipEdit, ip, ro);
            setEdit(m_hostEdit, host, ro);
            setEdit(m_commentEdit, comment, ro);
        }
        m_loading = false;
    }
    static void setEdit(HWND e, const std::string& s, bool ro)
    {
        if (!e)
            return;
        SetWindowTextW(e, wideFromUtf8(s).c_str());
        SendMessageW(e, EM_SETREADONLY, ro ? TRUE : FALSE, 0);
    }
    static std::string join(const std::vector<std::string>& v)
    {
        std::string out;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i)
                out += ' ';
            out += v[i];
        }
        return out;
    }

    // Write the detail controls back into the model. One edit session produces
    // exactly one undo step: EN_CHANGE keeps writing the working copy but only
    // the first change of a session pushes an undo snapshot. The session ends
    // after 400 ms of inactivity (kTimerCommit), on row change, or on save.
    void commitDetail()
    {
        if (m_visible.empty() || !rowEditable(m_sel))
            return;
        const int idx = m_visible[m_sel];
        if (m_kind == "env") {
            auto& r = m_env.rows[idx];
            const std::string name = getEdit(m_nameEdit);
            const std::string oldRef = r.scope + "/" + r.name;
            std::string value = m_listMode ? r.value : getEdit(m_valueEdit);
            auto candidate = m_partSessions.find(oldRef);
            if (!m_listMode && candidate != m_partSessions.end()) {
                if (value != partValue(candidate->second.current)) {
                    m_valueParts = envh::edit::splitDelimited(value, m_listDelimiter, true);
                    resetPartState();
                    rememberPartChange();
                }
                value = r.value;
            }
            std::string type = getEdit(m_typeEdit);
            if (type.empty())
                type = envh::envTypeForValue(value);
            if (r.name == name && r.value == value && r.type == type)
                return;
            if (!m_editActive) {
                m_env.pushUndo();
                m_editActive = true;
            }
            if (r.name != name) {
                auto node = m_partSessions.extract(oldRef);
                if (!node.empty()) {
                    node.key() = r.scope + "/" + name;
                    m_partSessions.insert(std::move(node));
                }
                m_partRef = r.scope + "/" + name;
            }
            r.name = name;
            r.value = value;
            r.type = type;
        } else {
            auto& r = m_hosts.rows[idx];
            const std::string ip = getEdit(m_ipEdit), comment = getEdit(m_commentEdit);
            const auto hostnames = splitWs(getEdit(m_hostEdit));
            if (r.ip == ip && r.comment == comment && r.hostnames == hostnames)
                return;
            if (!m_editActive) {
                m_hosts.pushUndo();
                m_editActive = true;
            }
            r.ip = ip;
            r.hostnames = hostnames;
            r.comment = comment;
        }
        // Do not rebuildVisible() here: renaming under an active filter would
        // make the row disappear mid-typing. drawRow() reads the model directly.
        if (m_hwnd) {
            SetTimer(m_hwnd, kTimerCommit, 400, nullptr);
            InvalidateRect(m_hwnd, nullptr, TRUE);
        }
    }
    void endEdit()
    {
        m_editActive = false;
        if (m_hwnd)
            KillTimer(m_hwnd, kTimerCommit);
    }
    static std::string getEdit(HWND e)
    {
        if (!e)
            return {};
        const int n = GetWindowTextLengthW(e);
        std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0)
            GetWindowTextW(e, w.data(), n + 1);
        return utf8FromWide(w);
    }
    static std::vector<std::string> splitWs(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    // ---- v0.6.0 always-on bands and the read-only diff overlay -------------
    void drawNotices(HDC dc, HFONT font)
    {
        const std::vector<Notice> ns = notices();
        SetRectEmpty(&m_rcBandDiff);
        SetRectEmpty(&m_rcBandReload);
        for (std::size_t i = 0; i < ns.size(); ++i) {
            const int top = m_rcTable.top + m_headerH + static_cast<int>(i) * kBandH;
            const RECT b {m_rcTable.left, top, m_rcTable.right, top + kBandH};
            HBRUSH bb = CreateSolidBrush(ns[i].bg);
            FillRect(dc, &b, bb);
            DeleteObject(bb);
            RECT bt = b;
            bt.left += 10;
            // Reserve the action zone (two links + relative time).
            bt.right -= (ns[i].action ? 300 : 10);
            SetTextColor(dc, ns[i].fg);
            HGDIOBJ of = SelectObject(dc, font);
            DrawTextW(dc, wideFromUtf8(ns[i].text).c_str(), -1, &bt,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (ns[i].action) {
                const int wide = ns[i].action == 2 ? 182 : 118;
                RECT ct = b;
                ct.left = b.right - wide - 86;
                ct.right = b.right - 86;
                m_rcBandDiff = ct;
                DrawTextW(dc, m_args.zh ? L"查看差异" : L"View diff", -1, &ct,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (ns[i].action == 2) {
                    RECT rt = b;
                    rt.left = b.right - 84;
                    rt.right = b.right - 10;
                    m_rcBandReload = rt;
                    DrawTextW(dc, m_args.zh ? L"重新载入" : L"Reload", -1, &rt,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                // "N 秒前": the watcher stamps m_extSeenTick when it fires.
                if (m_extSeenTick) {
                    const ULONGLONG secs = (GetTickCount64() - m_extSeenTick) / 1000;
                    const std::wstring ago = m_args.zh
                        ? std::to_wstring(secs) + L" 秒前"
                        : std::to_wstring(secs) + L" s ago";
                    RECT at = b;
                    at.left = m_rcBandDiff.left - 74;
                    at.right = m_rcBandDiff.left - 8;
                    DrawTextW(dc, ago.c_str(), -1, &at,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
            SelectObject(dc, of);
        }
    }

    void drawDiffPanel(HDC dc, HFONT ui, HFONT small, COLORREF fg, COLORREF dim)
    {
        const RECT& p = m_rcDiffPanel;
        HBRUSH bg = CreateSolidBrush(th().kDiffBg);
        FillRect(dc, &p, bg);
        DeleteObject(bg);
        HPEN ep = CreatePen(PS_SOLID, 2, th().kAccentDeep);
        HGDIOBJ op = SelectObject(dc, ep);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, p.left, p.top, p.right, p.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(ep);

        HGDIOBJ of = SelectObject(dc, ui);
        RECT t {p.left + 18, p.top + 8, p.right - 250, p.top + 34};
        SetTextColor(dc, fg);
        // Design title: 外部变化 · 差异  (right: N 秒前检测到 · 只读展示)
        DrawTextW(dc, m_args.zh ? L"外部变化 · 差异" : L"External change · Diff",
                  -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);

        HGDIOBJ osf = SelectObject(dc, small);
        RECT rt {p.right - 246, p.top + 8, p.right - 18, p.top + 34};
        SetTextColor(dc, dim);
        std::wstring when = m_args.zh ? L"只读展示" : L"read-only";
        if (m_extSeenTick) {
            const ULONGLONG secs = (GetTickCount64() - m_extSeenTick) / 1000;
            when = (m_args.zh ? std::to_wstring(secs) + L" 秒前检测到 · 只读展示"
                              : std::to_wstring(secs) + L" s ago · read-only");
        }
        DrawTextW(dc, when.c_str(), -1, &rt,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Amber band: what happened and how much of it needs a decision.
        const RECT band {p.left + 18, p.top + 40, p.right - 18, p.top + 62};
        HBRUSH bb = CreateSolidBrush(th().kWarnBg);
        FillRect(dc, &band, bb);
        DeleteObject(bb);
        SetTextColor(dc, th().kWarnText);
        RECT bt = band;
        bt.left += 10;
        const int total = m_kind == "env" ? static_cast<int>(m_diff.entries.size())
                                          : m_diffHosts.added + m_diffHosts.removed;
        std::wstring msg = m_args.zh ? L"环境已被外部修改" : L"Changed outside this window";
        msg += m_args.zh
            ? L" · " + std::to_wstring(total) + L" 处变化，其中 "
              + std::to_wstring(m_kind == "env" ? m_diff.conflicts : 0) + L" 处需要你决定"
            : L" · " + std::to_wstring(total) + L" change(s), "
              + std::to_wstring(m_kind == "env" ? m_diff.conflicts : 0) + L" need a decision";
        DrawTextW(dc, msg.c_str(), -1, &bt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // Three summary chips, as in the design.
        const int chipY = p.top + 68, chipH = 24;
        const int chipW = (p.right - p.left - 36 - 16) / 3;
        struct ChipDef {
            std::wstring text;
            COLORREF bg, fg;
        };
        const COLORREF cBg = th().kHeadBg;
        const ChipDef chips[3] = {
            {(m_args.zh ? std::to_wstring(total) + L" 处变化" : std::to_wstring(total) + L" changes"),
             cBg, th().kChipBorder},
            {(m_args.zh ? std::to_wstring(m_kind == "env" ? m_diff.conflicts : 0) + L" 处真冲突"
                        : std::to_wstring(m_kind == "env" ? m_diff.conflicts : 0) + L" real conflict(s)"),
             th().kChipDangerBg,
             th().kChipDangerFg},
            {(m_args.zh ? std::to_wstring(m_kind == "env" ? m_diff.theirsOnly : total) + L" 处仅外部改"
                        : std::to_wstring(m_kind == "env" ? m_diff.theirsOnly : total) + L" theirs only"),
             th().kChipInfoBg,
             th().kChipInfoFg},
        };
        for (int i = 0; i < 3; ++i) {
            const RECT cr {p.left + 18 + i * (chipW + 8), chipY,
                           p.left + 18 + i * (chipW + 8) + chipW, chipY + chipH};
            HBRUSH cb = CreateSolidBrush(chips[i].bg);
            FillRect(dc, &cr, cb);
            DeleteObject(cb);
            SetTextColor(dc, chips[i].fg);
            RECT ct = cr;
            DrawTextW(dc, chips[i].text.c_str(), -1, &ct,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        // Column header band: 变量 | 我的（未保存） | 外部最新 | 判定
        int y = chipY + chipH + 8;
        const RECT hb {p.left + 18, y, p.right - 18, y + 20};
        HBRUSH hbb = CreateSolidBrush(th().kHeadBg);
        FillRect(dc, &hb, hbb);
        DeleteObject(hbb);
        const int colName = hb.left + 8;
        const int colMine = hb.left + 150;
        const int colTheirs = hb.left + 330;
        const int colVerdict = hb.right - 8;
        SetTextColor(dc, dim);
        {
            RECT r {colName, hb.top, colMine - 6, hb.bottom};
            DrawTextW(dc, m_args.zh ? L"变量" : L"Name", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r = {colMine, hb.top, colTheirs - 6, hb.bottom};
            DrawTextW(dc, m_args.zh ? L"我的（未保存）" : L"Mine (unsaved)", -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r = {colTheirs, hb.top, colVerdict - 76, hb.bottom};
            DrawTextW(dc, m_args.zh ? L"外部最新" : L"Theirs (latest)", -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r = {colVerdict - 72, hb.top, colVerdict, hb.bottom};
            DrawTextW(dc, m_args.zh ? L"判定" : L"Verdict", -1, &r,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        y += 20;

        const int rowH = 24;
        const int until = m_rcDiffReload.top - 8;
        if (m_kind == "env") {
            if (m_diff.entries.empty()) {
                RECT e {p.left + 26, y, p.right - 26, y + rowH};
                DrawTextW(dc, m_args.zh ? L"（没有可显示的差异）" : L"(no differences to show)",
                          -1, &e, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            for (std::size_t i = static_cast<std::size_t>(m_diffScroll);
                 i < m_diff.entries.size() && y + rowH <= until; ++i, y += rowH) {
                const auto& e = m_diff.entries[i];
                const bool conflict = e.verdict == "conflict";
                const bool theirs = e.verdict == "theirs";
                const wchar_t* label = conflict
                    ? (m_args.zh ? L"真冲突" : L"CONFLICT")
                    : e.verdict == "mine" ? (m_args.zh ? L"仅我改" : L"mine only")
                    : theirs ? (m_args.zh ? L"仅外部改" : L"theirs only")
                    : (m_args.zh ? L"一致" : L"same");
                const COLORREF vc = conflict
                    ? (th().kDiffRed)
                    : theirs ? (th().kDiffBlue)
                    : dim;
                const RECT row {p.left + 18, y, p.right - 18, y + rowH};
                if (conflict) {
                    HBRUSH rb = CreateSolidBrush(th().kChipDangerBg);
                    FillRect(dc, &row, rb);
                    DeleteObject(rb);
                } else if (theirs) {
                    HBRUSH rb = CreateSolidBrush(th().kChipInfoBg);
                    FillRect(dc, &row, rb);
                    DeleteObject(rb);
                }
                if (conflict || theirs) {
                    RECT bar {row.left, row.top, row.left + 3, row.bottom};
                    HBRUSH bz = CreateSolidBrush(conflict
                        ? (th().kDangerSoft)
                        : (th().kSelBar));
                    FillRect(dc, &bar, bz);
                    DeleteObject(bz);
                }
                const std::string mine = e.mine.empty() && !e.base.empty() ? std::string("—") : e.mine;
                const std::string th = e.theirs.empty() && !e.base.empty() ? std::string("—") : e.theirs;
                SetTextColor(dc, conflict ? vc : fg);
                RECT r {colName, y, colMine - 6, y + rowH};
                DrawTextW(dc, wideFromUtf8(e.key).c_str(), -1, &r,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SetTextColor(dc, conflict ? vc : dim);
                r = {colMine, y, colTheirs - 6, y + rowH};
                DrawTextW(dc, wideFromUtf8(mine).c_str(), -1, &r,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SetTextColor(dc, theirs || conflict ? vc : dim);
                r = {colTheirs, y, colVerdict - 76, y + rowH};
                DrawTextW(dc, wideFromUtf8(th).c_str(), -1, &r,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SetTextColor(dc, vc);
                r = {colVerdict - 72, y, colVerdict, y + rowH};
                DrawTextW(dc, label, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            for (std::size_t i = static_cast<std::size_t>(m_diffScroll);
                 i < m_diffHosts.samples.size() && y + rowH <= until; ++i, y += rowH) {
                RECT lr {p.left + 26, y, p.right - 26, y + rowH};
                SetTextColor(dc, fg);
                DrawTextW(dc, wideFromUtf8(m_diffHosts.samples[i]).c_str(), -1, &lr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }

        // Footer hint under the actions. v0.7.2: the rect comes from
        // computeLayout() and is 22px tall, so the whole line is visible
        // instead of being sliced by the panel border.
        RECT ft = m_rcDiffFooter;
        SetTextColor(dc, dim);
        DrawTextW(dc, m_args.zh
                         ? L"不自动合并 · 不逐行采纳 · 每 2 秒刷新 · 选择前不会写入任何内容"
                         : L"No auto-merge · no per-row adoption · refreshes every 2 s · nothing is written before you choose",
                  -1, &ft, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, osf);

        drawFlatButton(dc, small, m_rcDiffReload,
                       m_args.zh ? L"重新载入（丢弃我的改动）" : L"Reload (discard mine)",
                       true, false, 60, fg, dim);
        drawFlatButton(dc, small, m_rcDiffForce,
                       m_args.zh ? L"强制覆盖（保存我的）" : L"Overwrite (save mine)",
                       true, false, 61, fg, dim);
        drawFlatButton(dc, small, m_rcDiffDefer,
                       m_args.zh ? L"暂不处理" : L"Defer", true, false, 62, fg, dim);
        SetTextColor(dc, fg);
    }

    // ---- paint ----
    void paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(m_hwnd, &ps);
        RECT rc {};
        GetClientRect(m_hwnd, &rc);
        const int w = rc.right, h = rc.bottom;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(th().kBg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);

        const COLORREF fg = th().kText;
        const COLORREF dim = th().kTextDim;
        const COLORREF accent = th().kAccent;
        const COLORREF selBg = th().kSelBg;

        // Fonts must actually be selected into the DC. Selecting them once and
        // restoring the original before deletion avoids leaking GDI handles.
        HFONT uiFont = makeFont(13, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        HFONT monoFont = makeFont(13, false, L"Consolas", FIXED_PITCH | FF_MODERN);
        HFONT smallFont = makeFont(11, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        HGDIOBJ oldFont = SelectObject(mem, uiFont);

        // tabs
        std::wstring tabs[3] = {
            m_kind == "env" ? (m_args.zh ? L"用户" : L"User")
                             : (m_args.zh ? L"条目" : L"Entries"),
            m_kind == "env" ? (m_args.zh ? L"系统" : L"System")
                            : (m_args.zh ? L"原文" : L"Raw"),
            m_args.zh ? L"备份" : L"Backup"};
        int nFirst = 0, nSecond = 0;
        if (m_kind == "env") {
            for (const auto& r : m_env.rows) {
                if (r.deleted) continue;
                if (r.scope == "user") ++nFirst; else ++nSecond;
            }
        } else {
            for (const auto& r : m_hosts.rows)
                if (!r.isSection && !r.deleted) ++nFirst;
            nSecond = static_cast<int>(m_hosts.toParsed().lines.size()); // real file lines
        }
        tabs[0] += L" (" + std::to_wstring(nFirst) + L")";
        tabs[1] += L" (" + std::to_wstring(nSecond) + L")";
        // v0.7.1: the backup count lives in the tab itself - the separate
        // "备份 (N)" pill in the detail header is gone.
        tabs[2] += L" (" + std::to_wstring(m_backupTotal) + L")";
        for (int i = 0; i < 3; ++i) {
            const bool active = i == m_tab;
            HBRUSH tb = CreateSolidBrush(active ? accent : (th().kTabBg));
            FillRect(mem, &m_rcTabs[i], tb);
            DeleteObject(tb);
            SetTextColor(mem, active ? th().kCommitFg : fg);
            DrawTextW(mem, tabs[i].c_str(), -1, &m_rcTabs[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Backup scope switch (env only): a segmented control right-aligned
        // in the tab strip. It filters BOTH the backup list and the preview.
        if (m_kind == "env" && m_tab == 2) {
            static const wchar_t* const zhLabels[3] = {L"全部", L"用户", L"系统"};
            static const wchar_t* const enLabels[3] = {L"All", L"User", L"System"};
            HGDIOBJ osf = SelectObject(mem, smallFont);
            RECT lbl = m_rcBackupScopeLabel;
            SetTextColor(mem, dim);
            DrawTextW(mem, m_args.zh ? L"备份范围" : L"Scope", -1, &lbl,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            for (int i = 0; i < 3; ++i) {
                const bool on = m_backupScope == i;
                HBRUSH sb = CreateSolidBrush(on ? accent : th().kChipBg);
                FillRect(mem, &m_rcBackupScope[i], sb);
                DeleteObject(sb);
                HPEN sp = CreatePen(PS_SOLID, 1, on ? accentEdge() : th().kChipEdge);
                HGDIOBJ osp = SelectObject(mem, sp);
                HGDIOBJ osb = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Rectangle(mem, m_rcBackupScope[i].left, m_rcBackupScope[i].top,
                          m_rcBackupScope[i].right, m_rcBackupScope[i].bottom);
                SelectObject(mem, osb);
                SelectObject(mem, osp);
                DeleteObject(sp);
                SetTextColor(mem, on ? th().kCommitFg : fg);
                DrawTextW(mem, m_args.zh ? zhLabels[i] : enLabels[i], -1, &m_rcBackupScope[i],
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(mem, osf);
        }

        // filter caption (the edit control itself is a real child window).
        // On the backup page the box moves into the preview pane as the
        // search, so the top-strip caption would label nothing.
        if (m_tab != 2) {
            RECT fr = m_rcFilterLabel;
            SetTextColor(mem, dim);
            HGDIOBJ of = SelectObject(mem, smallFont);
            DrawTextW(mem, m_args.zh ? L"过滤 Ctrl+F" : L"Filter Ctrl+F", -1, &fr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, of);
            SetTextColor(mem, fg);
        }

        // The error banner is painted last (see the header-band overlay near
        // the end of paint()). Drawing it here put it underneath the column
        // header, which starts at m_rcTable.top = 34: only a thin red line was
        // left visible and the message itself was unreadable.

        // ---- table: header band, column separators, rows ----
        if (m_tab != 2) {
            // The column header band was reserved (m_headerH) but never drawn,
            // leaving an unexplained empty strip above the first row.
            HGDIOBJ ohf = SelectObject(mem, smallFont);
            drawHeader(mem, fg, dim);
            SelectObject(mem, ohf);
            HGDIOBJ of = SelectObject(mem, monoFont);
            int y = m_rcTable.top + m_headerH + bannerH();
            if (m_visible.empty()) {
                SetTextColor(mem, dim);
                RECT er {m_rcTable.left + 12, y + 8, m_rcTable.right - 12, y + 8 + m_rowH};
                const wchar_t* emptyMsg = m_filterText.empty()
                    ? (m_args.zh ? L"（此分组没有条目，按 Ctrl+N 新增）" : L"(no entries here; press Ctrl+N to add)")
                    : (m_args.zh ? L"（没有匹配的条目）" : L"(no matching entries)");
                DrawTextW(mem, emptyMsg, -1, &er, DT_LEFT | DT_SINGLELINE);
            }
            const int first = std::min(m_topRow, maxTopRow());
            const int last = std::min<int>(rowCount(), first + pageRows());
            for (int v = first; v < last; ++v) {
                const int rowY = m_rcTable.top + m_headerH + bannerH() + (v - first) * m_rowH;
                const RECT row {m_rcTable.left, rowY, m_rcTable.right, rowY + m_rowH};
                const bool multi = isRowSelected(v);
                const COLORREF multiBg = th().kMultiSelBg;
                // Per-row dirty marking (v0.6.0): the row that actually has an
                // unsaved change gets its own colour and a right-edge label.
                const int mi = m_visible[v];
                const envh::edit::RowState st = m_kind == "env"
                    ? m_env.rowState(static_cast<std::size_t>(mi))
                    : m_hosts.rowState(static_cast<std::size_t>(mi));
                const bool over = m_kind == "env"
                    && envh::envValueOverlength(m_env.rows[mi].value) > 0;
                const bool pending = m_kind == "env" && envRowPending(static_cast<std::size_t>(mi));
                const bool marked = pending || over || st != envh::edit::RowState::Unchanged;
                const COLORREF dirtyBg = over ? th().kOverBg : pending ? th().kWarnBg : th().kDirtyBg;
                const COLORREF bg = v == m_sel ? selBg
                    : multi ? multiBg
                    : marked ? dirtyBg
                    : (v % 2 ? th().kStripe : th().kBg);
                HBRUSH rb = CreateSolidBrush(bg);
                FillRect(mem, &row, rb);
                DeleteObject(rb);
                if (marked) {
                    RECT bar {row.left, row.top, row.left + 3, row.bottom};
                    HBRUSH db = CreateSolidBrush(pending && !over ? th().kWarnText : th().kDanger);
                    FillRect(mem, &bar, db);
                    DeleteObject(db);
                }
                if (v == m_sel && multi) {
                    // Distinguish the focus row from the rest of a selection.
                    RECT bar {row.left + (marked ? 3 : 0), row.top,
                              row.left + (marked ? 6 : 3), row.bottom};
                    HBRUSH ab = CreateSolidBrush(th().kFocusBar);
                    FillRect(mem, &bar, ab);
                    DeleteObject(ab);
                }
                drawRow(mem, fg, v, row);
                if (marked) {
                    const wchar_t* tag = nullptr;
                    if (m_kind == "env" && (pending || st != envh::edit::RowState::Unchanged))
                        tag = envStateLabel(st, pending);
                    else if (over)
                        tag = m_args.zh ? L"⚠ 超限" : L"⚠ OVER";
                    else if (st == envh::edit::RowState::Added)
                        tag = m_args.zh ? L"+ 新增" : L"+ NEW";
                    else if (st == envh::edit::RowState::Modified)
                        tag = m_args.zh ? L"● 已改" : L"● EDIT";
                    else if (st == envh::edit::RowState::Deleted)
                        tag = m_args.zh ? L"− 删除" : L"− DEL";
                    if (tag) {
                        // The tag lives in the 状态 column (env) so the value
                        // column is not painted over.
                        RECT lr = m_kind == "env"
                            ? RECT {m_colX[3] + 4, rowY, m_colX[4] - 6, rowY + m_rowH}
                            : RECT {m_rcTable.right - 96, rowY, m_rcTable.right - 4, rowY + m_rowH};
                        HBRUSH lb = CreateSolidBrush(bg);
                        FillRect(mem, &lr, lb);
                        DeleteObject(lb);
                        SetTextColor(mem, pending ? th().kWarnText : th().kOverText);
                        HGDIOBJ otf = SelectObject(mem, smallFont);
                        DrawTextW(mem, tag, -1, &lr,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                        SelectObject(mem, otf);
                    }
                }
                // row separator
                HPEN lp = CreatePen(PS_SOLID, 1, th().kBorderSoft);
                HGDIOBJ op = SelectObject(mem, lp);
                MoveToEx(mem, row.left, row.bottom - 1, nullptr);
                LineTo(mem, row.right, row.bottom - 1);
                SelectObject(mem, op);
                DeleteObject(lp);
            }
            SelectObject(mem, of);
            // table frame
            HPEN fp = CreatePen(PS_SOLID, 1, th().kBorder);
            HGDIOBJ ofp = SelectObject(mem, fp);
            MoveToEx(mem, m_rcTable.left, m_rcTable.top, nullptr);
            LineTo(mem, m_rcTable.right, m_rcTable.top);
            LineTo(mem, m_rcTable.right, m_rcTable.bottom);
            LineTo(mem, m_rcTable.left, m_rcTable.bottom);
            LineTo(mem, m_rcTable.left, m_rcTable.top);
            SelectObject(mem, ofp);
            DeleteObject(fp);
        } else {
            HGDIOBJ of = SelectObject(mem, monoFont);
            drawBackupList(mem, smallFont, smallFont, fg, dim);
            SelectObject(mem, of);
        }

        // ---- detail panel: frame + column labels aligned to the edit controls ----
        if (m_tab != 2 && !(m_kind == "hosts" && m_tab == 1)) {
            HBRUSH db = CreateSolidBrush(th().kPanel);
            FillRect(mem, &m_rcDetail, db);
            DeleteObject(db);
            HPEN dp = CreatePen(PS_SOLID, 1, th().kBorder);
            HGDIOBJ odp = SelectObject(mem, dp);
            MoveToEx(mem, m_rcDetail.left, m_rcDetail.top, nullptr);
            LineTo(mem, m_rcDetail.right, m_rcDetail.top);
            LineTo(mem, m_rcDetail.right, m_rcDetail.bottom);
            LineTo(mem, m_rcDetail.left, m_rcDetail.bottom);
            LineTo(mem, m_rcDetail.left, m_rcDetail.top);
            SelectObject(mem, odp);
            DeleteObject(dp);
            HGDIOBJ of = SelectObject(mem, smallFont);
            SetTextColor(mem, dim);
        const int lx = m_rcDetail.left + 4;
        const int dw = m_rcDetail.right - lx - 4 - 200; // matches applyLayout's control row
            if (m_kind == "env") {
                RECT a {lx, m_rcDetail.top + 2, lx + dw / 2 - 6, m_rcDetail.top + 18};
                RECT b {lx + dw / 2 + 4, a.top, m_rcDetail.right - 4, a.bottom};
                RECT valueLabel {lx, m_rcDetail.top + 74, m_rcDetail.right - 4, m_rcDetail.top + 88};
                DrawTextW(mem, m_args.zh ? L"名称" : L"Name", -1, &a, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, m_args.zh ? L"类型" : L"Type", -1, &b, DT_LEFT | DT_SINGLELINE);
                if (currentRowDeleted() && !m_listMode) {
                    // A deleted row is read-only until restored; say so instead
                    // of leaving greyed controls unexplained.
                    SetTextColor(mem, th().kDeleted);
                    DrawTextW(mem,
                              m_args.zh ? L"整行已标记删除：保存时会从注册表移除该变量及其全部分隔项 · 按 Del 恢复后才能编辑"
                                        : L"Whole row marked deleted: saving removes this variable and ALL its split items · press Del to restore",
                              -1, &valueLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SetTextColor(mem, dim);
                } else if (!m_listMode) {
                    DrawTextW(mem, m_args.zh ? L"原文 · Ctrl+R 分隔项" : L"Raw value · Ctrl+R items",
                              -1, &valueLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            } else {
                SetTextColor(mem, fg);
                RECT title = m_rcHostTitle;
                DrawTextW(mem, m_args.zh ? L"主机映射" : L"Host mapping", -1, &title,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                RECT enabled = m_rcHostEnabledLabel;
                RECT ip = m_rcHostIpLabel;
                RECT hostname = m_rcHostNameLabel;
                RECT comment = m_rcHostCommentLabel;
                DrawTextW(mem, m_args.zh ? L"状态" : L"Status", -1, &enabled, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, L"IP", -1, &ip, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, m_args.zh ? L"主机名" : L"Hostnames", -1, &hostname, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, m_args.zh ? L"注释" : L"Comment", -1, &comment, DT_LEFT | DT_SINGLELINE);
            }
            SelectObject(mem, of);
        }

        // ---- draggable splitter between the env list and its detail panel ----
        if (m_rcSplitter.right > m_rcSplitter.left) {
            HBRUSH sb = CreateSolidBrush(th().kStatusBg);
            FillRect(mem, &m_rcSplitter, sb);
            DeleteObject(sb);
            HPEN gp = CreatePen(PS_SOLID, 1, th().kGridPen);
            HGDIOBJ ogp = SelectObject(mem, gp);
            const int cx = (m_rcSplitter.left + m_rcSplitter.right) / 2;
            const int cy = (m_rcSplitter.top + m_rcSplitter.bottom) / 2;
            for (int d = -14; d <= 14; d += 7) {
                MoveToEx(mem, cx + d - 2, cy, nullptr);
                LineTo(mem, cx + d + 2, cy);
            }
            SelectObject(mem, ogp);
            DeleteObject(gp);
        }

        // ---- detail action buttons + two-row status bar ----
        drawActionButtons(mem, smallFont, fg, dim);
        // v0.7.1: the "备份 (N)" pill is gone - the count moved into the
        // backup tab itself (see the tab strip above).
        if (m_kind == "env" && m_tab != 2) {
            drawModeSegment(mem, smallFont, fg, dim);
            const bool enabled = m_listMode && rowEditable(m_sel);
            if (!m_delimCustom) {
                // Same flat style as 应用 / the mode segment: label + caret.
                const std::wstring label = delimiterTextForValue(m_listDelimiter);
                drawFlatButton(mem, smallFont, m_rcDelimiter, L"", enabled, false, kFlatDelim, fg, dim);
                RECT lt = m_rcDelimiter;
                lt.left += 10;
                lt.right -= 22;
                SetTextColor(mem, enabled ? fg : dim);
                HGDIOBJ odf = SelectObject(mem, smallFont);
                DrawTextW(mem, label.c_str(), -1, &lt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                RECT ct = m_rcDelimiter;
                ct.left = ct.right - 22;
                DrawTextW(mem, L"▾", -1, &ct, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(mem, odf);
            }
            // "+ Add item" replaced the old "Apply" button: delimiter presets
            // apply on selection and a custom delimiter applies with Enter.
            drawFlatButton(mem, smallFont, m_rcApply,
                           m_args.zh ? L"+ 新增项" : L"+ Add item",
                           enabled, false, kFlatPartInsert, fg, dim);
            // Apply this variable's candidates; registry writes still require Save.
            {
                const int n = stagedPartCount();
                const auto pending = m_partSessions.find(currentRowRef());
                const bool canCommit = pending != m_partSessions.end() && partPending(pending->second);
                std::wstring label = m_args.zh ? L"提交修改" : L"Commit";
                if (n > 0)
                    label += L" (" + std::to_wstring(n) + L")";
                drawCommitButton(mem, smallFont, m_rcCommitBtn, label, canCommit && rowEditable(m_sel));
            }
            // The counter is a parent-painted label. Paint its own opaque panel
            // strip first so a child-control repaint cannot cut through it.
            RECT hint = m_rcItemHint;
            HBRUSH hintBg = CreateSolidBrush(th().kPanel);
            FillRect(mem, &hint, hintBg);
            DeleteObject(hintBg);
            hint.left += 4;
            HGDIOBJ of = SelectObject(mem, smallFont);
            if (!m_visible.empty() && m_sel >= 0 && m_sel < static_cast<int>(m_visible.size())) {
                const std::string value = m_listMode
                    ? joinedPartsValue()
                    : m_env.rows[m_visible[m_sel]].value;
                const std::size_t used = envh::utf16Units(value);
                const bool over = used > envh::kMaxEnvValue;
                const bool nearCap = !over && used > envh::kMaxEnvValue * 9 / 10;
                SetTextColor(mem, over ? (th().kOverText)
                                  : nearCap ? (th().kWarnText)
                                  : dim);
                const std::wstring counts = wideFromUtf8(
                    formatCount(used) + " / " + formatCount(envh::kMaxEnvValue)
                    + (m_args.zh ? " 字符" : " chars"));
                const std::wstring full = (m_args.zh ? L"共 " : L"")
                    + std::to_wstring(m_listMode ? m_valueParts.size() : 1)
                    + (m_args.zh ? L" 项 · 合计 " : L" item(s) · ")
                    + counts;
                // v0.7.2: the counter is left-aligned, like the design draft.
                // Right-aligned into a squeezed strip it used to lose the
                // "共 N 项 · 合计 " prefix and show only "9 / 32,767 字符".
                // If the full line still does not fit, the character count
                // wins - it is the number to read before saving. Do not apply
                // ellipsis to the complete form: a one-pixel rounding
                // difference must not hide the last glyph.
                const std::wstring shortText = (m_args.zh ? L"合计 " : L"") + counts;
                SIZE need {};
                GetTextExtentPoint32W(mem, full.c_str(), static_cast<int>(full.size()), &need);
                const bool fullFits = need.cx <= hint.right - hint.left;
                const wchar_t* text = fullFits ? full.c_str() : shortText.c_str();
                DrawTextW(mem, text, -1, &hint,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
                              | (fullFits ? 0 : DT_END_ELLIPSIS));
            }
            SelectObject(mem, of);
            SetTextColor(mem, fg);
            if (m_listMode)
                drawPartHeader(mem, smallFont, fg, dim);
        }
        // Always-on notice bands. They sit BELOW the column header and ABOVE
        // the rows (pageRows()/rowY reserve the height), so nothing can cover
        // the text. No DT_END_ELLIPSIS: the full message is always rendered.
        drawNotices(mem, smallFont);
        if (m_diffOpen)
            drawDiffPanel(mem, uiFont, smallFont, fg, dim);
        drawStatusBar(mem, smallFont, fg, dim);

        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(uiFont);
        DeleteObject(monoFont);
        DeleteObject(smallFont);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(m_hwnd, &ps);
    }

    bool isDirty() const { return m_kind == "env" ? m_envDirty() : (m_hostsDirty() || rawDirty()); }
    bool canUndo() const {
        auto it = m_partSessions.find(currentRowRef());
        return m_kind == "env" ? (m_env.canUndo() || (it != m_partSessions.end() && !it->second.undo.empty())) : m_hosts.canUndo();
    }
    bool canRedo() const {
        auto it = m_partSessions.find(currentRowRef());
        return m_kind == "env" ? (m_env.canRedo() || (it != m_partSessions.end() && !it->second.redo.empty())) : m_hosts.canRedo();
    }

    // ---- multi-selection ---------------------------------------------------
    std::size_t modelRowCount() const
    {
        return m_kind == "env" ? m_env.rows.size() : m_hosts.rows.size();
    }
    void clearSelection()
    {
        m_selected.clear();
        m_rangeAnchor = m_sel;
    }
    void ensureSelection()
    {
        if (m_selected.size() != modelRowCount())
            m_selected.assign(modelRowCount(), 0);
    }
    bool selectableRow(int v) const
    {
        if (v < 0 || v >= static_cast<int>(m_visible.size()) || m_tab == 2)
            return false;
        if (m_kind == "hosts" && m_tab == 0) {
            const int mi = m_visible[v];
            return !m_hosts.rows[mi].isSection; // group headings are display-only
        }
        return true;
    }
    bool isRowSelected(int v) const
    {
        if (v < 0 || v >= static_cast<int>(m_visible.size()))
            return false;
        const int mi = m_visible[v];
        return mi >= 0 && static_cast<std::size_t>(mi) < m_selected.size() && m_selected[mi] != 0;
    }
    int selectionCount() const
    {
        int n = 0;
        for (int v = 0; v < static_cast<int>(m_visible.size()); ++v)
            if (isRowSelected(v))
                ++n;
        return n;
    }
    void markVisible(int v, bool on)
    {
        ensureSelection();
        if (!selectableRow(v))
            return;
        m_selected[m_visible[v]] = on ? 1 : 0;
    }
    void applySelectionGesture(int v, bool additive, bool range)
    {
        ensureSelection();
        if (range) {
            const int a = std::max(0, std::min(m_rangeAnchor, v));
            const int b = std::min(static_cast<int>(m_visible.size()) - 1, std::max(m_rangeAnchor, v));
            for (int i = a; i <= b; ++i)
                markVisible(i, true);
        } else if (additive) {
            const bool was = isRowSelected(v);
            markVisible(v, !was);
            m_rangeAnchor = v;
        } else {
            std::fill(m_selected.begin(), m_selected.end(), 0);
            m_rangeAnchor = v;
            markVisible(v, true);
        }
        selectRow(v);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    void selectAllVisible()
    {
        ensureSelection();
        std::fill(m_selected.begin(), m_selected.end(), 0);
        for (int v = 0; v < static_cast<int>(m_visible.size()); ++v)
            markVisible(v, true);
        m_rangeAnchor = m_sel;
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    // Shift+arrow: replace the selection with the range [anchor, target] and
    // move the focus row, without ever changing the anchor.
    void extendSelectionTo(int next)
    {
        ensureSelection();
        const int anchor = m_rangeAnchor;
        const int target = std::max(0, std::min(next, static_cast<int>(m_visible.size()) - 1));
        std::fill(m_selected.begin(), m_selected.end(), 0);
        const int a = std::min(anchor, target), b = std::max(anchor, target);
        for (int i = a; i <= b; ++i)
            markVisible(i, true);
        selectRow(target);
        m_rangeAnchor = anchor;
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    // Visible rows only: a selection hidden by a filter must never be written
    // to by a batch operation. Falls back to the focus row for single-select.
    std::vector<int> selectedModelIndices() const
    {
        std::vector<int> out;
        if (m_tab == 2 || (m_kind == "hosts" && m_tab == 1))
            return out;
        for (int v = 0; v < static_cast<int>(m_visible.size()); ++v)
            if (isRowSelected(v))
                out.push_back(m_visible[v]);
        if (out.empty() && m_sel >= 0 && m_sel < static_cast<int>(m_visible.size()))
            out.push_back(m_visible[m_sel]);
        return out;
    }
    bool multiSelectActive() const { return selectionCount() > 1; }

    bool rowEditable(int v) const
    {
        if (m_tab == 2 || v < 0 || v >= static_cast<int>(m_visible.size()))
            return false;
        const int i = m_visible[v];
        if (m_kind == "env")
            return (m_env.rows[i].scope == "user" || (m_env.rows[i].scope == "system" && m_args.systemWritable))
                   && !m_env.rows[i].deleted;
        if (m_tab == 1 || !m_args.writable)
            return false;
        return !m_hosts.rows[i].isSection && !m_hosts.rows[i].deleted;
    }
    bool canAddRow() const
    {
        if (m_tab == 2)
            return false;
        if (m_kind == "env")
            return m_tab == 0 || (m_tab == 1 && m_args.systemWritable);
        return m_tab == 0 && m_args.writable;
    }

    std::string statusFlashText() const
    {
        if (m_statusFlash.empty() || GetTickCount64() > m_statusFlashUntil)
            return {};
        return m_statusFlash;
    }

    std::string statusRow1Text() const
    {
        const std::string flash = statusFlashText();
        if (!flash.empty())
            return flash;
        const bool zh = m_args.zh;
        // Design footer for the item table:
        // ● N 项未保存 · 共 M 项 · 合计 X / 32,767 字符 · 用户 / 系统均可写
        if (m_kind == "env" && m_listMode && m_tab != 2) {
            int unsaved = 0;
            for (int i = 0; i < static_cast<int>(m_valueParts.size()); ++i) {
                const bool added = i < static_cast<int>(m_partNew.size())
                                   && m_partNew[static_cast<std::size_t>(i)];
                if (added || partChanged(i) || partStaged(i))
                    ++unsaved;
            }
            const auto candidate = m_partSessions.find(currentRowRef());
            const bool pending = candidate != m_partSessions.end() && partPending(candidate->second);
            const std::size_t used = envh::utf16Units(joinedPartsValue());
            const bool changed = !m_visible.empty()
                && m_env.rowState(m_visible[m_sel]) != envh::edit::RowState::Unchanged;
            std::string s = pending
                ? (zh ? "● 有未提交的分隔项修改" : "● Uncommitted item changes")
                : changed ? (zh ? "● 当前变量未保存" : "● Current variable unsaved")
                          : (zh ? "○ 当前变量无修改" : "○ Current variable unchanged");
            if (pending && unsaved > 0)
                s += " (" + std::to_string(unsaved) + ")";
            if (pending && changed)
                s += zh ? " · 未保存" : " · Unsaved";
            if (stagedPartCount() > 0)
                s += zh ? " · " + std::to_string(stagedPartCount()) + " 项待删除"
                        : " · " + std::to_string(stagedPartCount()) + " staged";
            if (partSelectedCount() > 1)
                s += zh ? " · 已选 " + std::to_string(partSelectedCount()) + " 项"
                        : " · " + std::to_string(partSelectedCount()) + " selected";
            s += zh ? " · 共 " + std::to_string(m_valueParts.size()) + " 项"
                    : " · " + std::to_string(m_valueParts.size()) + " item(s)";
            s += zh ? " · 合计 " + formatCount(used) + " / " + formatCount(envh::kMaxEnvValue) + " 字符"
                    : " · " + formatCount(used) + " / " + formatCount(envh::kMaxEnvValue) + " chars";
            s += m_args.systemWritable ? (zh ? " · 管理员：用户 / 系统均可写" : " · admin: user and system writable")
                                       : (zh ? " · 用户可写 / 系统只读" : " · user writable / system read-only");
            return s;
        }
        const bool dirty = isDirty();
        std::string s = dirty ? (zh ? "● 有未保存修改" : "● unsaved changes")
                              : (zh ? "○ 无修改" : "○ no changes");
        if (dirty) {
            const int n = m_kind == "env" ? static_cast<int>(m_env.changeList().size())
                                          : static_cast<int>(m_hosts.changeList().size());
            if (n > 0)
                s += " · " + std::to_string(n) + (zh ? " 项变更" : " changes");
        }
        const bool rawTab = m_kind == "hosts" && m_tab == 1;
        const int nRows = rowCount();
        if (rawTab) {
            const auto lc = rawCaretLine();
            s += zh ? " · 第 " + std::to_string(lc.first) + " 行 / 共 " + std::to_string(lc.second) + " 行"
                    : " · line " + std::to_string(lc.first) + " / " + std::to_string(lc.second);
            if (rawDirty())
                s += zh ? " · 原文已修改" : " · raw text edited";
        } else if (nRows > 0) {
            const int a = m_topRow + 1;
            const int b = std::min(nRows, m_topRow + pageRows());
            s += zh ? (" · 第 " + std::to_string(a) + "–" + std::to_string(b) + " / 共 " + std::to_string(nRows) + " 行")
                    : (" · rows " + std::to_string(a) + "-" + std::to_string(b) + " / " + std::to_string(nRows));
        } else {
            s += zh ? " · 0 行" : " · 0 rows";
        }
        if (const int n = selectionCount(); n > 1)
            s += zh ? " · 已选 " + std::to_string(n) + " 行" : " · " + std::to_string(n) + " selected";
        if (m_skippedSelected > 0)
            s += zh ? " · 跳过 " + std::to_string(m_skippedSelected) + " 行只读"
                    : " · " + std::to_string(m_skippedSelected) + " read-only skipped";
        if (!m_filterText.empty())
            s += zh ? " · 过滤 " + m_filterText : " · filter " + m_filterText;
        if (m_extChanged)
            s += zh ? " · 外部已修改" : " · external change";
        s += m_kind == "env"
                 ? (m_args.systemWritable ? (zh ? " · 管理员：用户 / 系统均可写" : " · admin: user and system writable")
                                          : (zh ? " · 用户可写 / 系统只读" : " · user writable / system read-only"))
                 : (m_args.writable ? (zh ? " · 可写" : " · writable")
                                    : (zh ? " · 只读（需管理员）" : " · read-only (needs admin)"));
        if (m_kind == "hosts") {
            const std::size_t dup = m_hosts.duplicateCount();
            if (dup > 0)
                s += zh ? " · " + std::to_string(dup) + " 行主机名重复（!）"
                        : " · " + std::to_string(dup) + " duplicate host line(s) (!)";
        }
        return s;
    }

    // The "ⓘ" click on status row 1: explains where the plugin's write
    // permissions come from and how to get administrator rights (Plan A: the
    // `elevated` permission follows the host; the host never shows UAC for a
    // plugin, so the two steps below are the only way up).
    void showPermissionHelp()
    {
        const bool zh = m_args.zh;
        const bool writable = m_kind == "env" ? m_args.systemWritable : m_args.writable;
        std::string body;
        if (writable) {
            body = zh
                       ? "当前以管理员权限运行：本窗口内的目标（" + std::string(m_kind == "env" ? "用户 / 系统环境变量" : "hosts") + "）均可编辑并保存。\n\n这份权限来自 Win顺 的 elevated 插件机制，不是本插件弹出的 UAC。"
                       : "The plugin currently runs elevated: everything in this window (" + std::string(m_kind == "env" ? "user and system variables" : "hosts") + ") can be edited and saved.\n\nThis comes from WinShun's elevated-plugin mechanism, not a UAC prompt from this plugin.";
        } else {
            body = zh
                       ? std::string("当前为受限权限：") + (m_kind == "env" ? "只能修改用户环境变量，系统变量只读。" : "hosts 为只读。")
                             + "\n\n要以管理员权限编辑：\n  1. 以管理员身份运行 Win顺；\n  2. 在 设置 → 插件 的 envhosts 卡片上点一次“确认”，插件会以管理员权限重新启动。\n\n说明：Win顺 从不为插件弹 UAC；插件文件被改动后需要重新确认；提权后的插件不响应 AI / 命令行调用。"
                       : std::string("Restricted permissions right now: ") + (m_kind == "env" ? "only user variables are writable, system variables are read-only." : "hosts is read-only.")
                             + "\n\nTo edit with administrator rights:\n  1. Run WinShun as administrator;\n  2. click \"Confirm\" once on the envhosts card in Settings → Plugins; the plugin restarts elevated.\n\nNotes: WinShun never shows UAC for a plugin; the confirmation must be repeated after the plugin files change; an elevated plugin ignores AI / command-line calls.";
        }
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"权限说明" : L"Permissions";
        ds.subtitle = m_args.zh ? L"为什么某些变量无法编辑" : L"Why some entries cannot be edited";
        ds.body = wideFromUtf8(body);
        ds.icon = envh::dlg::kInfo;
        ds.buttons = {{m_args.zh ? L"知道了" : L"Got it", IDOK, envh::dlg::kPlain}};
        ds.defButton = 0;
        envh::dlg::show(m_hwnd, ds, th(), m_dark);
    }

    const wchar_t* chipLabel(int id) const
    {
        const bool zh = m_args.zh;
        switch (id) {
        case kChipAdd: return zh ? L"Ctrl+N 新增" : L"Ctrl+N add";
        case kChipDel: return (m_kind == "env" && m_listMode)
                           ? (zh ? L"Ctrl+U 删除整行" : L"Ctrl+U del row")
                           : currentRowDeleted()
                               ? (zh ? L"Del 恢复" : L"Del restore")
                               : (zh ? L"Del 删除" : L"Del delete");
        case kChipEdit: return zh ? L"Enter 编辑" : L"Enter edit";
        case kChipUndo: return zh ? L"Ctrl+Z 撤销" : L"Ctrl+Z undo";
        case kChipRedo: return zh ? L"Ctrl+Y 重做" : L"Ctrl+Y redo";
        case kChipSave: return zh ? L"Ctrl+S 保存" : L"Ctrl+S save";
        case kChipClose: return zh ? L"Esc 关闭" : L"Esc close";
        case kChipPartUp: return zh ? L"上移 Alt+↑" : L"Up Alt+↑";
        case kChipPartDown: return zh ? L"下移 Alt+↓" : L"Down Alt+↓";
        }
        return L"";
    }
    bool chipEnabled(int id) const
    {
        // In the raw tab the text box owns editing (its own Ctrl+Z / Ctrl+Y);
        // only save and close apply.
        if (m_kind == "hosts" && m_tab == 1)
            return id == kChipSave ? isDirty() : id == kChipClose;
        switch (id) {
        case kChipAdd: return canAddRow();
        case kChipDel: return canDeleteRow();
        case kChipEdit: return rowEditable(m_sel);
        case kChipUndo: return canUndo();
        case kChipRedo: return canRedo();
        case kChipSave: return isDirty();
        case kChipClose: return true;
        case kChipPartUp:
        case kChipPartDown:
            return m_kind == "env" && m_tab != 2 && m_listMode && rowEditable(m_sel)
                && m_partSel >= 0 && m_partSel < static_cast<int>(m_valueParts.size())
                && (id == kChipPartUp ? m_partSel > 0 : m_partSel + 1 < static_cast<int>(m_valueParts.size()));
        }
        return false;
    }

    enum FlatId {
        kFlatModeRaw = 10,
        kFlatModeList = 11,
        kFlatApply = 30,
        kFlatDelim = 31,
        kFlatPartUp = 41,
        kFlatPartDown = 42,
        kFlatPartInsert = 43,
        kFlatPartDelete = 44,
        kFlatCommit = 45,
        kFlatBandDiff = 61,
        kFlatBandReload = 62
    };
    static bool rectHasPoint(const RECT& r, POINT p)
    {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }
    int hitFlat(POINT p) const
    {
        if (m_kind != "env" || m_tab == 2)
            return 0;
        if (rectHasPoint(m_rcModeRaw, p))
            return kFlatModeRaw;
        if (rectHasPoint(m_rcModeList, p))
            return kFlatModeList;
        if (rectHasPoint(m_rcCommitBtn, p))
            return kFlatCommit;
        if (m_listMode && rowEditable(m_sel) && rectHasPoint(m_rcApply, p))
            return kFlatPartInsert; // the "+ Add item" toolbar button
        if (!m_delimCustom && m_listMode && rowEditable(m_sel) && rectHasPoint(m_rcDelimiter, p))
            return kFlatDelim;
        // v0.6.0: no side strip any more; the per-row × lives in the item table
        // and is handled by the list box subclass.
        return 0;
    }
    // Rounded pill helper. The two pills it used to draw (备份 (N) in the
    // detail header, 返回编辑 on the backup page) are both gone in v0.7.0/1 -
    // the count lives in the tab strip. Kept for future flat chrome.
    void drawPill(HDC dc, HFONT font, const RECT& r, const std::wstring& text, COLORREF fg)
    {
        if (r.right <= r.left || r.bottom <= r.top)
            return;
        const COLORREF bg = th().kFocusRing;
        const COLORREF edge = th().kPillEdge;
        HBRUSH b = CreateSolidBrush(bg);
        HGDIOBJ ob = SelectObject(dc, b);
        HPEN p = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ op = SelectObject(dc, p);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 12, 12);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(p);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        HGDIOBJ of = SelectObject(dc, font);
        RECT t = r;
        DrawTextW(dc, text.c_str(), -1, &t,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }

    // v0.7.0: the green "提交修改" button of the two-stage delete flow. A
    // distinct helper because drawFlatButton has no custom-fill variant.
    void drawCommitButton(HDC dc, HFONT font, const RECT& r, const std::wstring& label, bool enabled)
    {
        if (r.left >= r.right || r.top >= r.bottom)
            return;
        const bool hovered = enabled && m_hover == kFlatCommit;
        const COLORREF bg = !enabled ? th().kBtnBg
            : hovered ? th().kCommitBr
            : th().kCommitBg;
        HBRUSH b = CreateSolidBrush(bg);
        FillRect(dc, &r, b);
        DeleteObject(b);
        HPEN p = CreatePen(PS_SOLID, 1, enabled ? th().kCommitBr : th().kBtnEdgeDim);
        HGDIOBJ op = SelectObject(dc, p);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(p);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? th().kCommitFg : th().kTextMuted);
        HGDIOBJ of = SelectObject(dc, font);
        RECT t = r;
        DrawTextW(dc, label.c_str(), -1, &t,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }

    void drawFlatButton(HDC dc, HFONT font, const RECT& r, const wchar_t* label, bool enabled,
                        bool active, int id, COLORREF fg, COLORREF dim)
    {
        if (r.left >= r.right || r.top >= r.bottom)
            return;
        const bool hovered = enabled && m_hover == id;
        COLORREF bg;
        if (!enabled)
            bg = th().kBtnBg;
        else if (active)
            bg = accent();
        else if (hovered)
            bg = th().kBtnActiveBg;
        else
            bg = th().kBtnHoverBg;
        HBRUSH b = CreateSolidBrush(bg);
        FillRect(dc, &r, b);
        DeleteObject(b);
        HPEN p = CreatePen(PS_SOLID, 1, enabled
                                      ? (active ? accentEdge() : (th().kBtnEdge))
                                      : (th().kBtnEdgeDim));
        HGDIOBJ old = SelectObject(dc, p);
        // Rectangle() fills with the selected brush (white by default), which
        // used to wipe the accent fill and leave white text on white.
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, old);
        DeleteObject(p);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? (active ? th().kCommitFg : fg) : dim);
        HGDIOBJ of = SelectObject(dc, font);
        RECT t = r;
        DrawTextW(dc, label, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }
    void drawModeSegment(HDC dc, HFONT font, COLORREF fg, COLORREF dim)
    {
        // A segment control is one visual unit: only the selected half receives
        // accent background, while both halves keep identical metrics.
        drawFlatButton(dc, font, m_rcModeRaw, m_args.zh ? L"原文" : L"Raw", true, !m_listMode,
                       kFlatModeRaw, fg, dim);
        drawFlatButton(dc, font, m_rcModeList, m_args.zh ? L"分隔项" : L"Items", true, m_listMode,
                       kFlatModeList, fg, dim);
    }
    COLORREF accent() const { return th().kAccent; }
    COLORREF accentEdge() const { return th().kAccentDeep; }

    void drawActionButtons(HDC dc, HFONT font, COLORREF fg, COLORREF dim)
    {
        if (m_tab == 2 || (m_kind == "hosts" && m_tab == 1))
            return;
        const wchar_t* labels[4] = {
            m_args.zh ? L"+ 新增" : L"+ Add",
            m_args.zh ? L"- 删除" : L"- Del",
            m_args.zh ? L"撤销" : L"Undo",
            m_args.zh ? L"重做" : L"Redo"};
        RECT rects[4] = {m_rcAddBtn, m_rcDelBtn, m_rcUndoBtn, m_rcRedoBtn};
        const bool en[4] = {canAddRow(), canDeleteRow(), canUndo(), canRedo()};
        for (int i = 0; i < 4; ++i)
            drawFlatButton(dc, font, rects[i], labels[i], en[i], false, 0, fg, dim);
    }

    void drawStatusBar(HDC dc, HFONT font, COLORREF fg, COLORREF dim)
    {
        const bool flash = !statusFlashText().empty();
        const int kind = m_statusFlashKind;
        COLORREF bg1 = th().kStatusRow1Bg;
        COLORREF t1 = m_extChanged ? th().kWarn : fg;
        if (flash) {
            bg1 = kind == 2 ? th().kChipDangerBg
                : kind == 1 ? th().kWarnBg
                : kind == 3 ? th().kSuccessBg
                            : (th().kStatusSelBg);
            t1 = kind == 2 ? th().kChipDangerFg
               : kind == 1 ? th().kWarnText
               : kind == 3 ? th().kSuccessText
                           : (th().kChipTextStrong);
        }
        HBRUSH b1 = CreateSolidBrush(bg1);
        FillRect(dc, &m_rcStatusRow1, b1);
        DeleteObject(b1);
        HBRUSH b2 = CreateSolidBrush(th().kStatusRow2Bg);
        FillRect(dc, &m_rcStatusRow2, b2);
        DeleteObject(b2);
        HGDIOBJ of = SelectObject(dc, font);
        SetTextColor(dc, t1);
        RECT r1 = m_rcStatusRow1;
        r1.left += 8;
        // The permission entry lives at the right end of row 1 as a small flat
        // button (plain text: the U+24D8 glyph is missing from some CJK fonts,
        // so a drawn character would risk rendering as a box).
        const bool elevatedNow = m_kind == "env" ? m_args.systemWritable : m_args.writable;
        const LONG infoW = 46;
        const RECT ri {m_rcStatusRow1.right - infoW - 8, m_rcStatusRow1.top + 3,
                       m_rcStatusRow1.right - 8, m_rcStatusRow1.bottom - 3};
        m_rcInfo = ri;
        r1.right = ri.left;
        DrawTextW(dc, wideFromUtf8(statusRow1Text()).c_str(), -1, &r1,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        HBRUSH ib = CreateSolidBrush(th().kChipBg);
        FillRect(dc, &ri, ib);
        DeleteObject(ib);
        HBRUSH ie = CreateSolidBrush(elevatedNow ? (th().kChipElevBg)
                                                  : (th().kElevText));
        FrameRect(dc, &ri, ie);
        DeleteObject(ie);
        RECT it = ri;
        SetTextColor(dc, elevatedNow ? (th().kChipTextStrong)
                                     : (th().kModified));
        DrawTextW(dc, m_args.zh ? L"权限" : L"Perms",
                  -1, &it, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        for (Chip c : m_chips) {
            const bool en = chipEnabled(c.id);
            HBRUSH cb = CreateSolidBrush(en ? (th().kChipBg)
                                            : (th().kChipOffBg));
            FillRect(dc, &c.rc, cb);
            DeleteObject(cb);
            HBRUSH edge = CreateSolidBrush(en ? (th().kChipElevBg)
                                              : (th().kChipEdge));
            FrameRect(dc, &c.rc, edge);
            DeleteObject(edge);
            SetTextColor(dc, en ? fg : dim);
            DrawTextW(dc, chipLabel(c.id), -1, &c.rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SetTextColor(dc, dim);
        DrawTextW(dc, m_args.zh ? L"分隔项：" : L"Items:", -1, &m_rcPartGroup,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }

    void drawHeader(HDC dc, COLORREF fg, COLORREF dim)
    {
        const RECT band {m_rcTable.left, m_rcTable.top, m_rcTable.right, m_rcTable.top + m_headerH};
        HBRUSH hb = CreateSolidBrush(th().kSubHeadBg);
        FillRect(dc, &band, hb);
        DeleteObject(hb);
        SetTextColor(dc, dim);
        const wchar_t* labels[5] = {
            m_kind == "env" ? (m_args.zh ? L"名称" : L"Name") : (m_args.zh ? L"启用" : L"On"),
            m_kind == "env" ? (m_args.zh ? L"类型" : L"Type") : L"IP",
            m_kind == "env" ? (m_args.zh ? L"值" : L"Value") : (m_args.zh ? L"主机名" : L"Hostname"),
            m_kind == "env" ? (m_args.zh ? L"状态" : L"State") : (m_args.zh ? L"注释" : L"Comment"),
            m_args.zh ? L"行" : L"Line"};
        for (int c = 0; c < m_colCount; ++c) {
            RECT r {m_colX[c] + 6, band.top, m_colX[c + 1] - 4, band.bottom};
            DrawTextW(dc, labels[c], -1, &r,
                      (m_kind == "env" && c == 3 ? DT_RIGHT : DT_LEFT)
                          | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        HPEN p = CreatePen(PS_SOLID, 1, th().kBorder);
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, band.left, band.bottom - 1, nullptr);
        LineTo(dc, band.right, band.bottom - 1);
        for (int c = 1; c < m_colCount; ++c) {
            MoveToEx(dc, m_colX[c], band.top + 4, nullptr);
            LineTo(dc, m_colX[c], band.bottom - 4);
        }
        SelectObject(dc, op);
        DeleteObject(p);
        (void)fg;
    }

    void drawCell(HDC dc, const std::string& text, int c, const RECT& row, COLORREF color)
    {
        RECT r {m_colX[c] + 6, row.top, m_colX[c + 1] - 4, row.bottom};
        SetTextColor(dc, color);
        DrawTextW(dc, wideFromUtf8(text).c_str(), -1, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    void drawRow(HDC dc, COLORREF fg, std::size_t v, const RECT& row)
    {
        const COLORREF normal = fg;
        const COLORREF removed = th().kRemovedText;
        if (m_kind == "env") {
            const auto& r = m_env.rows[m_visible[v]];
            const COLORREF c = r.deleted ? removed : normal;
            drawCell(dc, r.name, 0, row, c);
            drawCell(dc, r.type, 1, row, c);
            drawCell(dc, r.value, 2, row, c);
        } else {
            const auto& r = m_hosts.rows[m_visible[v]];
            const COLORREF c = r.deleted ? removed : (r.commented ? (th().kMutedText) : normal);
            if (r.isSection) {
                // Group headings span the whole row: drawing them in column 0
                // (8% of the width) truncated every comment to "# Copy...".
                HBRUSH hb = CreateSolidBrush(th().kSectionBg);
                FillRect(dc, &row, hb);
                DeleteObject(hb);
                RECT sr {row.left + 8, row.top, row.right - 8, row.bottom};
                SetTextColor(dc, th().kTextDim);
                std::wstring text = wideFromUtf8(r.sectionText);
                if (r.isHeader) {
                    const std::string span = std::to_string(r.origin + 1) + "–" + std::to_string(m_hosts.headerEnd() + 1);
                    text = wideFromUtf8(m_args.zh ? "▸ 文件头注释（第 " + span + " 行，完整内容见“原文”页）"
                                                  : "▸ File header comments (lines " + span + "; see the Raw tab)");
                }
                DrawTextW(dc, text.c_str(), -1, &sr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            } else {
                drawCell(dc, r.commented ? "[ ]" : "[x]", 0, row, c);
                drawCell(dc, r.ip, 1, row, c);
                drawCell(dc, join(r.hostnames), 2, row, c);
                drawCell(dc, r.comment, 3, row, c);
                drawCell(dc, std::to_string(r.origin + 1), 4, row, c);
                const auto dup = m_hosts.duplicateFlags();
                if (static_cast<std::size_t>(m_visible[v]) < dup.size() && dup[m_visible[v]]) {
                    // A later line for an already-mapped host name is ignored
                    // by Windows; flag it at the right edge of the line column.
                    RECT mr {m_colX[5] - 22, row.top, m_colX[5] - 6, row.bottom};
                    SetTextColor(dc, removed);
                    DrawTextW(dc, L"!", -1, &mr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }
    }

    // ---- v0.7.0 backup page geometry --------------------------------------
    // The backup page has three stacked regions and each one used to compute
    // its own row origin: the left list drew from top+24, the right preview
    // drew from top+52+18, but the click test assumed top+4 for the list and
    // top+24 for the preview. Every click therefore landed on the wrong row
    // (or on nothing at all). One geometry source now feeds both painting
    // and hit testing.
    static constexpr int kBackupHeadH = 20; // column header row

    // Left pane: pane header (0..26), then 44px two-line entries.
    int backupListTop() const { return m_rcBackupList.top + 26; }
    int backupListRowH() const { return envh::theme::Metrics::kBackupListRowH; }
    int backupListHit(int y) const
    {
        if (y < backupListTop() || y >= m_rcBackupList.bottom)
            return -1;
        const int idx = m_topRow + (y - backupListTop()) / backupListRowH();
        return idx >= 0 && idx < rowCount() ? idx : -1;
    }

    // Right pane: header (0..24), search box (28..52), column header
    // (56..76), then standard rows.
    int backupPreviewHeadTop() const { return m_rcBackupPreview.top + 56; }
    int backupPreviewTop() const { return backupPreviewHeadTop() + kBackupHeadH; }
    int backupPreviewHit(int y) const
    {
        if (y < backupPreviewTop() || y >= m_rcBackupPreview.bottom - 24)
            return -1;
        return (y - backupPreviewTop()) / m_rowH;
    }

    // Rounded scope chip: 用户 (blue) / 系统 (purple), also used by the
    // design drafts (kScopeUser* / kScopeSys*).
    void drawScopeChip(HDC dc, HFONT font, const RECT& r, bool system)
    {
        if (r.right <= r.left || r.bottom <= r.top)
            return;
        const COLORREF bg = system ? th().kScopeSysBg : th().kScopeUserBg;
        const COLORREF fg = system ? th().kScopeSysFg : th().kScopeUserFg;
        const COLORREF br = system ? th().kScopeSysBr : th().kScopeUserBr;
        HBRUSH b = CreateSolidBrush(bg);
        HGDIOBJ ob = SelectObject(dc, b);
        HPEN p = CreatePen(PS_SOLID, 1, br);
        HGDIOBJ op = SelectObject(dc, p);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 10, 10);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(p);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        HGDIOBJ of = SelectObject(dc, font);
        RECT t = r;
        DrawTextW(dc, system ? (m_args.zh ? L"系统" : L"Sys") : (m_args.zh ? L"用户" : L"User"),
                  -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }

    // "HH:MM:SS" out of a backup id like 20261107T205602.526Z.
    std::wstring backupTimeLabel(const std::string& id) const
    {
        if (id.size() < 15 || id[8] != 'T')
            return wideFromUtf8(id);
        return wideFromUtf8(id.substr(9, 2) + ":" + id.substr(11, 2) + ":" + id.substr(13, 2));
    }

    void drawBackupList(HDC dc, HFONT font, HFONT small, COLORREF fg, COLORREF dim)
    {
        HGDIOBJ of = SelectObject(dc, font);
        auto frame = [&](const RECT& r) {
            HPEN p = CreatePen(PS_SOLID, 1, th().kBorder);
            HGDIOBJ op = SelectObject(dc, p);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(p);
        };
        frame(m_rcBackupList);
        frame(m_rcBackupPreview);

        // Left pane header: "备份 (N)" + retention hint.
        RECT lh {m_rcBackupList.left + 8, m_rcBackupList.top + 3,
                 m_rcBackupList.right - 8, m_rcBackupList.top + 23};
        SetTextColor(dc, fg);
        const std::wstring lht = (m_args.zh ? L"备份 (" : L"Backups (")
            + std::to_wstring(m_backupIds.size()) + L")";
        DrawTextW(dc, lht.c_str(), -1, &lh, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, small);
        SetTextColor(dc, th().kTextMuted);
        RECT rh2 {m_rcBackupList.right - 150, m_rcBackupList.top + 5,
                  m_rcBackupList.right - 8, m_rcBackupList.top + 23};
        DrawTextW(dc, m_args.zh ? L"自动保留最近 10 份" : L"keeps the latest 10", -1, &rh2,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (m_backupIds.empty()) {
            RECT rr {m_rcBackupList.left + 8, backupListTop() + 4,
                     m_rcBackupList.right - 8, backupListTop() + 4 + m_rowH};
            SetTextColor(dc, dim);
            DrawTextW(dc, m_args.zh ? L"还没有备份" : L"No backups yet", -1, &rr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            // Two-line entries drawn and hit-tested from ONE origin
            // (backupListTop); this is what used to be three disagreeing
            // numbers.
            const int first = std::min(m_topRow, maxTopRow());
            const int last = std::min<int>(rowCount(), first + pageRows());
            for (int i = first; i < last; ++i) {
                const int yy = backupListTop() + (i - first) * backupListRowH();
                const RECT rr {m_rcBackupList.left + 2, yy,
                               m_rcBackupList.right - 2, yy + backupListRowH()};
                if (i == m_sel) {
                    HBRUSH hb = CreateSolidBrush(th().kSelBg);
                    FillRect(dc, &rr, hb);
                    DeleteObject(hb);
                    HBRUSH bar = CreateSolidBrush(th().kSelBar);
                    const RECT bz {rr.left, rr.top, rr.left + 3, rr.bottom};
                    FillRect(dc, &bz, bar);
                    DeleteObject(bar);
                }
                // Line 1: backup id.
                SetTextColor(dc, fg);
                RECT tr = rr;
                tr.left += 10;
                tr.top += 2;
                tr.bottom = tr.top + 20;
                DrawTextW(dc, wideFromUtf8(m_backupIds[i]).c_str(), -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // Line 2: time · count.
                SetTextColor(dc, dim);
                const int n = i < static_cast<int>(m_backupCounts.size()) ? m_backupCounts[i] : 0;
                const std::wstring meta = backupTimeLabel(m_backupIds[i]) + L" · "
                    + std::to_wstring(n)
                    + (m_kind == "env" ? (m_args.zh ? L" 个变量" : L" var(s)")
                                       : (m_args.zh ? L" 行" : L" line(s)"));
                RECT mr = rr;
                mr.left += 10;
                mr.top += 22;
                mr.bottom = mr.top + 18;
                DrawTextW(dc, meta.c_str(), -1, &mr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // Scope chips at the right edge (user + system can coexist).
                const bool hasU = i < static_cast<int>(m_backupHasUser.size()) && m_backupHasUser[i];
                const bool hasS = i < static_cast<int>(m_backupHasSystem.size()) && m_backupHasSystem[i];
                int cx = rr.right - 8;
                if (hasS) {
                    const RECT cr {cx - 38, yy + 13, cx, yy + 30};
                    drawScopeChip(dc, small, cr, true);
                    cx -= 42;
                }
                if (hasU) {
                    const RECT cr {cx - 38, yy + 13, cx, yy + 30};
                    drawScopeChip(dc, small, cr, false);
                }
                // Row separator.
                HPEN lp = CreatePen(PS_SOLID, 1, th().kBorderSoft);
                HGDIOBJ op = SelectObject(dc, lp);
                MoveToEx(dc, rr.left, rr.bottom - 1, nullptr);
                LineTo(dc, rr.right, rr.bottom - 1);
                SelectObject(dc, op);
                DeleteObject(lp);
            }
        }

        // Right pane header: "<id>" + "<time> · N 个变量 · 与当前状态对比".
        RECT rh {m_rcBackupPreview.left + 8, m_rcBackupPreview.top + 3,
                 m_rcBackupPreview.right - 8, m_rcBackupPreview.top + 23};
        SelectObject(dc, font);
        SetTextColor(dc, fg);
        std::wstring ph;
        if (!m_backupIds.empty() && m_sel >= 0 && m_sel < static_cast<int>(m_backupIds.size())) {
            const int n = m_sel < static_cast<int>(m_backupCounts.size()) ? m_backupCounts[m_sel] : 0;
            ph = wideFromUtf8(m_backupIds[m_sel]) + L" · "
                 + std::to_wstring(n)
                 + (m_kind == "env" ? (m_args.zh ? L" 个变量 · 与当前状态对比"
                                                 : L" var(s) · vs current state")
                                    : (m_args.zh ? L" 行 · 与当前文件对比"
                                                 : L" lines · vs current file"));
        } else {
            ph = m_args.zh ? L"预览 · 与当前状态对比" : L"Preview · compared with the current state";
        }
        DrawTextW(dc, ph.c_str(), -1, &rh, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Column header: 变量 / 范围 / 备份中的值 / 差异. One origin
        // (backupPreviewHeadTop) feeds painting and backupPreviewHit.
        const int colHdrTop = backupPreviewHeadTop();
        const RECT ch {m_rcBackupPreview.left + 4, colHdrTop,
                       m_rcBackupPreview.right - 4, colHdrTop + kBackupHeadH};
        HBRUSH chb = CreateSolidBrush(th().kHeadBg);
        FillRect(dc, &ch, chb);
        DeleteObject(chb);
        SelectObject(dc, small);
        const int pName = ch.left + 6;
        const int pScope = ch.left + 150;
        const int pValue = ch.left + 200;
        const int pDiff = ch.right - 8;
        SetTextColor(dc, dim);
        {
            RECT r {pName, ch.top, pScope - 6, ch.bottom};
            DrawTextW(dc, m_args.zh ? L"变量" : L"Name", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r = {pScope, ch.top, pValue - 6, ch.bottom};
            DrawTextW(dc, m_args.zh ? L"范围" : L"Scope", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r = {pValue, ch.top, pDiff - 90, ch.bottom};
            DrawTextW(dc, m_args.zh ? L"备份中的值" : L"Value in backup", -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            r = {pDiff - 86, ch.top, pDiff, ch.bottom};
            DrawTextW(dc, m_args.zh ? L"差异" : L"Diff", -1, &r,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        if (!m_backupIds.empty() && m_sel >= 0 && m_sel < static_cast<int>(m_backupIds.size())) {
            const auto rows = backupPreview(m_backupIds[m_sel]);
            const int maxRows = std::max(0, static_cast<int>(
                m_rcBackupPreview.bottom - colHdrTop - kBackupHeadH - 24) / m_rowH);
            for (int i = 0; i < maxRows && i < static_cast<int>(rows.size()); ++i) {
                const int yy = backupPreviewTop() + i * m_rowH;
                const RECT rr {m_rcBackupPreview.left + 4, yy,
                               m_rcBackupPreview.right - 4, yy + m_rowH};
                if (i == m_backupPreviewSel) {
                    HBRUSH hb = CreateSolidBrush(th().kPreviewSelBg);
                    FillRect(dc, &rr, hb);
                    DeleteObject(hb);
                }
                const auto& row = rows[static_cast<std::size_t>(i)];
                const COLORREF mc = row.marker == "~" ? (th().kDiffValue)
                    : row.marker == "+" ? (th().kDiffSame)
                    : row.marker == "-" ? (th().kDiffExtra)
                    : dim;
                RECT nr {pName, yy, pScope - 6, rr.bottom};
                RECT vr {pValue, yy, pDiff - 90, rr.bottom};
                SetTextColor(dc, fg);
                DrawTextW(dc, wideFromUtf8(row.name).c_str(), -1, &nr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // Scope chip column; hosts rows carry no scope.
                if (row.scope == "user" || row.scope == "system") {
                    const RECT cr {pScope, yy + 3, pScope + 38, yy + 21};
                    drawScopeChip(dc, small, cr, row.scope == "system");
                }
                SetTextColor(dc, dim);
                DrawTextW(dc, wideFromUtf8(row.value).c_str(), -1, &vr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // Design writes the verdict as text, not a bare marker.
                const wchar_t* dtxt = row.marker == "~" ? (m_args.zh ? L"~ 值不同" : L"~ differs")
                    : row.marker == "+" ? (m_args.zh ? L"+ 仅备份有" : L"+ backup only")
                    : row.marker == "-" ? (m_args.zh ? L"- 当前多出" : L"- current only")
                    : (m_args.zh ? L"= 相同" : L"= same");
                RECT dr {pDiff - 86, yy, pDiff, rr.bottom};
                SetTextColor(dc, mc);
                DrawTextW(dc, dtxt, -1, &dr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                // Row separator.
                HPEN lp = CreatePen(PS_SOLID, 1, th().kBorderSoft);
                HGDIOBJ op = SelectObject(dc, lp);
                MoveToEx(dc, rr.left, rr.bottom - 1, nullptr);
                LineTo(dc, rr.right, rr.bottom - 1);
                SelectObject(dc, op);
                DeleteObject(lp);
            }
        }
        // Design footer: restore/delete semantics spelled out.
        RECT fh {m_rcBackupPreview.left + 8, m_rcBackupPreview.bottom - 20,
                 m_rcBackupPreview.right - 8, m_rcBackupPreview.bottom - 3};
        SetTextColor(dc, dim);
        DrawTextW(dc, m_args.zh
                         ? L"恢复前自动备份当前状态 · 二次确认 · 删除会移除该备份文件"
                         : L"Restore auto-backs-up first · confirmations · delete removes the file",
                  -1, &fh, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);

        const bool have = !m_backupIds.empty();
        drawFlatButton(dc, font, m_rcBackupPreviewBtn,
                       m_args.zh ? L"刷新预览" : L"Refresh", true, false, 0, fg, dim);
        drawFlatButton(dc, font, m_rcBackupRestoreBtn,
                       m_args.zh ? L"恢复…" : L"Restore…", have, false, 0, fg, dim);
        drawFlatButton(dc, font, m_rcBackupDeleteBtn,
                       m_args.zh ? L"删除" : L"Delete", have, false, 0, fg, dim);
        drawFlatButton(dc, font, m_rcBackupExportBtn,
                       m_args.zh ? L"导出 JSON" : L"Export JSON", have, false, 0, fg, dim);
    }

    // Only HARD errors live here: an invalid name, a control character in a
    // value, or an invalid hosts line. Since v0.6.0 an over-long env value is a
    // SOFT warning (see notices()); it must not silently disable the hosts raw
    // tab, which is why rawEditable() gates on this function alone.
    std::string m_errorBanner() const
    {
        if (m_kind == "env") {
            for (const auto& r : m_env.rows) {
                if (r.deleted || (r.scope == "system" && !m_args.systemWritable))
                    continue;
                if (r.name.empty() || !envh::isValidEnvName(r.name))
                    return m_args.zh ? "变量名无效：" + r.name : "Invalid name: " + r.name;
                if (!envh::envValueHardErrorOk(r.value))
                    return m_args.zh ? r.name + " 的值含控制字符，无法保存"
                                     : r.name + " contains control characters; cannot save";
            }
        } else if (m_hosts.hasInvalid()) {
            return m_args.zh ? "有无效行（检查 IP / 主机名）" : "Invalid rows (check IP / hostnames)";
        }
        return {};
    }

    std::vector<envh::edit::EnvWarning> envWarnings() const
    {
        return m_kind == "env" ? m_env.warnings(m_args.systemWritable)
                               : std::vector<envh::edit::EnvWarning> {};
    }

    // One always-on band per condition. These are drawn BELOW the column header
    // and ABOVE the rows (never inside the header band, where the text used to
    // be covered) and never with DT_END_ELLIPSIS.
    struct Notice {
        std::string text;
        COLORREF bg;
        // v0.7.2: the band foreground travels with the band. The bands are
        // dark and saturated, so one shared "warning text" colour made the
        // message unreadable (see kBandErrFg / kBandOverFg / kBandWarnFg).
        COLORREF fg;
        int action; // 1 = click opens the external-change diff panel
    };
    std::vector<Notice> notices() const
    {
        std::vector<Notice> out;
        if (m_tab == 2)
            return out;
        const std::string hard = m_errorBanner();
        if (!hard.empty())
            out.push_back({hard, th().kErrorBand, th().kBandErrFg, 0});

        const auto warns = envWarnings();
        if (!warns.empty()) {
            const auto& w = warns.front();
            const std::string used = std::to_string(w.used);
            const std::string cap = std::to_string(w.cap);
            const std::string over = std::to_string(w.over);
            std::string s = m_args.zh
                ? "⚠ " + w.name + " " + used + " / " + cap + " 字符（超出 " + over
                  + "）· 可以保存，但新进程可能读不到完整值；建议删除分隔项或缩短"
                : "⚠ " + w.name + " " + used + " / " + cap + " chars (" + over
                  + " over) · saving is allowed, but new processes may not read it back; remove items or shorten";
            if (warns.size() > 1)
                s += m_args.zh ? "（另有 " + std::to_string(warns.size() - 1) + " 项超限）"
                               : " (+" + std::to_string(warns.size() - 1) + " more over the cap)";
            out.push_back({s, th().kOverBand, th().kBandOverFg, 0});
        } else if (m_kind == "env" && !m_visible.empty() && m_sel >= 0
                   && m_sel < static_cast<int>(m_visible.size())) {
            // Amber "near the cap" band, so the warning appears BEFORE the
            // value actually breaks the limit.
            const auto& r = m_env.rows[m_visible[m_sel]];
            const std::size_t used = envh::utf16Units(r.value);
            if (used > envh::kMaxEnvValue * 9 / 10 && used <= envh::kMaxEnvValue) {
                out.push_back({m_args.zh
                    ? r.name + " " + std::to_string(used) + " / 32767 字符 · 接近 Windows 上限"
                    : r.name + " " + std::to_string(used) + " / 32767 chars · close to the Windows cap",
                    th().kWarn, th().kBandWarnFg, 0});
            }
        }
        if (m_extChanged) {
            const int n = m_kind == "env" ? static_cast<int>(m_diff.entries.size())
                                           : m_diffHosts.added + m_diffHosts.removed;
            // Design: "环境已被外部修改 · N 处变化", with 查看差异 / 重新载入
            // as separate actions and a relative timestamp on the right.
            const std::string s = m_args.zh
                ? "环境已被外部修改 · " + std::to_string(n) + " 处变化"
                : "Changed outside this window · " + std::to_string(n) + " difference(s)";
            out.push_back({s, th().kWarn, th().kBandWarnFg, 2});
        }
        return out;
    }
    int bannerH() const
    {
        return m_tab == 2 ? 0 : static_cast<int>(notices().size()) * kBandH;
    }

    // ---- external-change watcher ------------------------------------------
    static envh::EnvSnapshot writableScopes(const envh::EnvSnapshot& s, bool systemWritable)
    {
        envh::EnvSnapshot out;
        for (const auto& v : s.vars)
            if (v.scope == "user" || (systemWritable && v.scope == "system"))
                out.vars.push_back(v);
        return out;
    }

    void watchExternal()
    {
        // Never fight the user: a modal dialog, an active edit session or an
        // open diff panel all pause the watcher. Recomputing the diff while
        // typing would make the band flicker and could steal focus.
        if (m_diffOpen || m_modalOpen || m_tab == 2 || m_editActive
            || m_partInline >= 0 || !m_hwnd)
            return;
        if (m_kind == "env") {
            const bool sysW = m_args.systemWritable;
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            const envh::native::EnvReadResult r = store.read(sysW);
            if (!r.ok)
                return;
            const std::string fp = sysW ? envh::edit::EnvModel::computeFingerprint(r.snap)
                                        : envh::edit::EnvModel::computeUserFingerprint(r.snap);
            if (fp == (sysW ? m_env.fingerprint : m_env.userFingerprint)) {
                m_extFingerprint.clear();
                return;
            }
            if (fp == m_extFingerprint)
                return; // same external state already reported
            if (!isDirty()) {
                reloadFromDisk(); // clean editor: adopting the new state is safe
                return;
            }
            m_extFingerprint = fp;
            const envh::EnvSnapshot base = writableScopes(
                envh::edit::diff3RowsToSnapshot(m_env.base), sysW);
            const envh::EnvSnapshot mine = writableScopes(
                envh::edit::diff3RowsToSnapshot(m_env.rows), sysW);
            m_diff = envh::edit::diff3Env(base, mine, writableScopes(r.snap, sysW));
        } else {
            envh::native::HostsStore store(m_args.hostsPath);
            const envh::native::HostsReadResult r = store.read();
            if (!r.ok)
                return;
            const std::string fp = envh::edit::HostsModel::contentFingerprint(r.content);
            if (fp == m_hosts.fingerprint) {
                m_extFingerprint.clear();
                return;
            }
            if (fp == m_extFingerprint)
                return;
            if (!isDirty()) {
                reloadFromDisk();
                return;
            }
            m_extFingerprint = fp;
            m_diffHosts = envh::edit::diff3Hosts(envh::serializeHosts(m_hosts.disk), r.content);
        }
        m_extChanged = true;
        m_extSeenTick = GetTickCount64();
        m_diffScroll = 0;
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void openDiff()
    {
        if (m_kind == "env" && m_diff.entries.empty())
            watchExternal(); // build it on demand from the live target
        m_diffOpen = true;
        m_diffScroll = 0;
        // Child EDIT controls are native windows and always paint on top of
        // the parent DC; without this the detail fields cut holes into the
        // lower half of the panel and visually bury its buttons.
        for (HWND e : {m_nameEdit, m_typeEdit, m_valueEdit, m_partsList,
                       m_partEdit, m_delimEdit, m_rawEdit})
            if (e)
                ShowWindow(e, SW_HIDE);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    void closeDiff()
    {
        m_diffOpen = false;
        applyLayout(); // restore the normal detail controls
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    // The three whole-view actions. No per-row merge is ever performed.
    void diffReload()
    {
        closeDiff();
        reloadFromDisk();
        setStatus(m_args.zh ? "已重新载入外部最新值（我的改动已丢弃）"
                           : "Reloaded the external values (my edits discarded)", 1);
    }
    void diffForceOverwrite()
    {
        closeDiff();
        save(/*force=*/true);
    }
    void diffDefer()
    {
        closeDiff();
        setStatus(m_args.zh ? "暂不处理外部变化；保存前会再次提示" : "Deferred; you will be asked again before saving", 0);
    }

    // ---- backup page data (v0.6.0) ----------------------------------------
    struct BackupPreviewRow {
        std::string marker; // "=" | "~" | "+" | "-"
        std::string name;
        std::string value;
        std::string scope; // "user" | "system" | "" (hosts lines)
    };
    std::vector<BackupPreviewRow> backupPreview(const std::string& id) const
    {
        std::vector<BackupPreviewRow> out;
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        const std::string lf = envh::toLower(m_filterText);
        if (m_kind == "env") {
            envh::EnvSnapshot snap;std::set<std::string> scopes;std::string error;
            if (!backups.readEnvChecked(id, snap, scopes, error)) {
                out.push_back({"!", error, m_args.zh ? "请使用生成此备份的版本恢复" : "Restore with the edition that created this backup", ""});
                return out;
            }
            std::map<std::string, envh::EnvVar> current;
            for (const auto& r : m_env.rows)
                if (!r.deleted && !r.name.empty()) {
                    envh::EnvVar v;
                    v.scope = r.scope;
                    v.name = r.name;
                    v.value = r.value;
                    v.type = r.type;
                    current[envh::toLower(v.name)] = v;
                }
            for (const auto& v : snap.vars) {
                // The scope filter applies to the preview rows as well: a
                // "user only" view must not show system variables.
                if (m_backupScope == kScopeUser && v.scope == "system")
                    continue;
                if (m_backupScope == kScopeSystem && v.scope != "system")
                    continue;
                if (!lf.empty() && envh::toLower(v.name).find(lf) == std::string::npos
                    && envh::toLower(v.value).find(lf) == std::string::npos)
                    continue;
                const auto it = current.find(envh::toLower(v.name));
                const std::string marker = it == current.end() ? "+"
                    : (it->second.value == v.value && it->second.type == v.type) ? "=" : "~";
                out.push_back({marker, v.name, v.value, v.scope});
            }
            if (out.size() < 40) {
                for (const auto& kv : current) {
                    if (m_backupScope == kScopeUser && kv.second.scope == "system")
                        continue;
                    if (m_backupScope == kScopeSystem && kv.second.scope != "system")
                        continue;
                    bool inBackup = false;
                    for (const auto& v : snap.vars)
                        if (envh::toLower(v.name) == kv.first)
                            inBackup = true;
                    if (!inBackup && (lf.empty() || kv.first.find(lf) != std::string::npos))
                        out.push_back({"-", kv.second.name, kv.second.value, kv.second.scope});
                }
            }
        } else {
            const std::string content = backups.readHosts(id);
            const envh::HostsParsed p = envh::parseHosts(content);
            for (std::size_t i = 0; i < p.lines.size(); ++i) {
                const std::string& line = p.lines[i].text;
                if (line.empty())
                    continue;
                if (!lf.empty() && envh::toLower(line).find(lf) == std::string::npos)
                    continue;
                out.push_back({"=", std::to_string(i + 1), line, ""});
            }
        }
        return out;
    }
    void doBackupRestore()
    {
        if (m_tab != 2 || m_backupIds.empty() || m_sel < 0
            || m_sel >= static_cast<int>(m_backupIds.size()))
            return;
        const std::string id = m_backupIds[m_sel];
        m_modalOpen = true;
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"恢复备份" : L"Restore backup";
        ds.subtitle = wideFromUtf8(id);
        ds.body = wideFromUtf8(m_args.zh
            ? "恢复这份备份会在编辑器中生成未保存的改动。\n\n当前状态会先自动备份一份，确认后仍需按 Ctrl+S 写入。"
            : "Restoring creates unsaved edits in the editor.\n\nThe current state is backed up first; Ctrl+S is still required.");
        ds.icon = envh::dlg::kWarn;
        ds.buttons = {
            {m_args.zh ? L"取消" : L"Cancel", IDCANCEL, envh::dlg::kPlain},
            {m_args.zh ? L"恢复" : L"Restore", IDYES, envh::dlg::kPrimary},
        };
        ds.defButton = 0;
        const int answer = envh::dlg::show(m_hwnd, ds, th(), m_dark);
        m_modalOpen = false;
        if (answer != IDYES)
            return;
        {
            envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
            if (m_kind == "env") {
                envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
                backups.saveEnv(store.read(m_args.systemWritable).snap, m_args.systemWritable);
            } else {
                envh::native::HostsStore store(m_args.hostsPath);
                backups.saveHosts(store.read().content);
            }
        }
        loadBackupIntoWork(id);
    }
    void doBackupDelete()
    {
        if (m_tab != 2 || m_backupIds.empty() || m_sel < 0
            || m_sel >= static_cast<int>(m_backupIds.size()))
            return;
        const std::string id = m_backupIds[m_sel];
        m_modalOpen = true;
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"删除备份" : L"Delete backup";
        ds.subtitle = wideFromUtf8(id);
        ds.body = wideFromUtf8(m_args.zh ? "删除这份备份文件？此操作不可撤销。"
                                         : "Delete this backup file? This cannot be undone.");
        ds.icon = envh::dlg::kDanger;
        ds.buttons = {
            {m_args.zh ? L"取消" : L"Cancel", IDNO, envh::dlg::kPlain},
            {m_args.zh ? L"删除" : L"Delete", IDYES, envh::dlg::kDangerBtn},
        };
        ds.defButton = 0;
        const int answer = envh::dlg::show(m_hwnd, ds, th(), m_dark);
        m_modalOpen = false;
        if (answer != IDYES)
            return;
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        const bool ok = backups.remove(m_kind, id);
        refreshBackups();
        m_sel = std::min(m_sel, rowCount() - 1 < 0 ? 0 : rowCount() - 1);
        setStatus(ok ? (m_args.zh ? "已删除备份 " + id : "Deleted backup " + id)
                     : (m_args.zh ? "删除失败" : "Delete failed"), ok ? 3 : 2);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
    void doBackupExport()
    {
        if (m_tab != 2 || m_backupIds.empty() || m_sel < 0
            || m_sel >= static_cast<int>(m_backupIds.size()))
            return;
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        const std::string path = backups.exportTo(m_kind, m_backupIds[m_sel]);
        setStatus(path.empty()
                      ? (m_args.zh ? "导出失败" : "Export failed")
                      : (m_args.zh ? "已导出：" + path : "Exported: " + path),
                  path.empty() ? 2 : 3);
    }
    struct ModalGuard {
        EditorWindow* w;
        explicit ModalGuard(EditorWindow* win) : w(win) { w->m_modalOpen = true; }
        ~ModalGuard() { w->m_modalOpen = false; }
    };

    void save(bool force = false)
    {
        // The last keystroke may still be sitting in an EDIT control. Commit it
        // before validating, otherwise the check (and the write) run against
        // the previous value - this is how an overlong PATH looked "saveable
        // with no warning" in 0.4.0.
        if (m_listMode)
            commitPartEdit();
        commitDetail();
        endEdit();
        if (m_kind == "env")
            applyPartSessions(true);
        if (m_kind == "hosts" && m_tab == 1) {
            commitRaw();
            loadRaw(); // show the applied text so the box and the model agree
        }
        const bool dirty = isDirty();
        if (!dirty) {
            setStatus(m_args.zh ? "没有改动" : "No changes");
            return;
        }
        if (m_kind == "env" && m_env.hasInvalid(m_args.systemWritable)) {
            setStatus(m_args.zh ? "有硬错误，无法保存（检查名称/控制字符）"
                               : "Hard error; cannot save (check names / control chars)", 2);
            return;
        }
        if (m_kind == "hosts" && m_hosts.hasInvalid()) {
            setStatus(m_args.zh ? "有无效行，无法保存（检查 IP/主机名）" : "Invalid lines; cannot save", 2);
            return;
        }
        // Confirm with the actual change list, not just a count.
        std::vector<std::string> details;
        if (m_kind == "env") {
            for (const auto& c : m_env.changeList())
                details.push_back(c.detail);
        } else {
            for (const auto& c : m_hosts.changeList())
                details.push_back(c.detail);
        }
        std::vector<std::string> deletedNames;
        if (m_kind == "env") {
            for (const auto& c : m_env.changeList())
                if (c.kind == "delete")
                    deletedNames.push_back(c.name);
        } else {
            for (const auto& c : m_hosts.changeList())
                if (c.kind == "delete")
                    deletedNames.push_back(c.detail);
        }
        std::string body;
        if (!deletedNames.empty()) {
            std::string names;
            for (std::size_t i = 0; i < deletedNames.size() && i < 6; ++i)
                names += (i ? ", " : "") + deletedNames[i];
            if (deletedNames.size() > 6)
                names += " …";
            body += m_kind == "env"
                ? (m_args.zh
                    ? "⚠ 将删除 " + std::to_string(deletedNames.size())
                      + " 个变量：" + names + "\n\n"
                    : "⚠ " + std::to_string(deletedNames.size())
                      + " variable(s) will be DELETED: " + names + "\n\n")
                : (m_args.zh
                    ? "⚠ 将删除 " + std::to_string(deletedNames.size())
                      + " 个 hosts 行：" + names + "\n\n"
                    : "⚠ " + std::to_string(deletedNames.size())
                      + " hosts line(s) will be DELETED: " + names + "\n\n");
        }
        const auto warns = envWarnings();
        if (!warns.empty()) {
            std::string names;
            for (std::size_t i = 0; i < warns.size() && i < 3; ++i)
                names += (i ? ", " : "") + warns[i].name;
            body += m_args.zh
                ? "⚠ 超长警告：" + names + " 超过 32767 字符。"
                  "注册表可写入，但新进程可能读不到完整值，甚至整条丢失。\n\n"
                : "⚠ OVER-LENGTH: " + names + " exceeds 32767 characters."
                  " The registry accepts it, but new processes may not read the full value back.\n\n";
        }
        body += (m_args.zh ? "保存 " : "Save ") + std::to_string(details.size())
                           + (m_args.zh ? " 项修改：" : " changes:");
        // The per-change list goes into the monospace detail block; the body
        // keeps the headline so test drivers reading ctrl 0xFFFF still see
        // "将删除 N 个变量" / the over-length warning.
        std::string detailList;
        for (std::size_t i = 0; i < details.size() && i < 8; ++i)
            detailList += details[i] + "\n";
        if (details.size() > 8)
            detailList += "... +" + std::to_string(details.size() - 8) + "\n";
        ModalGuard modal {this};
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"确认保存" : L"Confirm save";
        ds.subtitle = m_args.zh ? L"写入注册表前的最后一步" : L"Last step before writing to the registry";
        ds.body = wideFromUtf8(body + "\n\n"
                                + std::string(m_args.zh ? "确认写入？" : "Write now?"));
        if (!detailList.empty())
            ds.detail = wideFromUtf8(detailList);
        ds.icon = warns.empty() && deletedNames.empty() ? envh::dlg::kInfo
                  : deletedNames.empty() ? envh::dlg::kWarn
                                         : envh::dlg::kDanger;
        ds.buttons = {
            {m_args.zh ? L"取消" : L"Cancel", IDNO, envh::dlg::kPlain},
            {m_args.zh ? L"确认保存" : L"Save", IDYES, envh::dlg::kPrimary},
        };
        ds.defButton = 0;
        if (envh::dlg::show(m_hwnd, ds, th(), m_dark) != IDYES)
            return;

        std::unique_lock<std::mutex> lock(envh::native::g_writeLock);
        if (m_kind == "env") {
            const bool sysW = m_args.systemWritable;
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            // With an elevated plugin the system scope is written too, so a
            // change there is a conflict as well; otherwise only HKCU counts.
            const envh::native::EnvReadResult liveUser = store.read(sysW);
            if (!liveUser.ok) {
                setStatus(m_args.zh ? ("读取失败：" + liveUser.errMsg) : ("Read failed: " + liveUser.errMsg), 2);
                return;
            }
            // Only the scopes this window can write count as a conflict.
            // Comparing the full system+user snapshot while HKLM is read-only
            // made an unrelated HKLM edit cancel every save.
            const std::string fpNow = sysW ? envh::edit::EnvModel::computeFingerprint(liveUser.snap)
                                           : envh::edit::EnvModel::computeUserFingerprint(liveUser.snap);
            if (!force && fpNow != (sysW ? m_env.fingerprint : m_env.userFingerprint)) {
                // v0.6.0: the old MB_ABORTRETRYIGNORE dialog is gone. The
                // read-only diff panel shows exactly what changed and offers
                // one whole-view decision; nothing is merged automatically.
                // Release the write lock first: the panel can stay open for a
                // long time and must not block the protocol thread.
                lock.unlock();
                m_extFingerprint = fpNow;
                const envh::EnvSnapshot base = writableScopes(
                    envh::edit::diff3RowsToSnapshot(m_env.base), sysW);
                const envh::EnvSnapshot mine = writableScopes(
                    envh::edit::diff3RowsToSnapshot(m_env.rows), sysW);
                m_diff = envh::edit::diff3Env(base, mine, writableScopes(liveUser.snap, sysW));
                m_extChanged = true;
                m_extSeenTick = GetTickCount64();
                openDiff();
                InvalidateRect(m_hwnd, nullptr, TRUE);
                setStatus(m_args.zh ? "检测到外部修改，已打开只读差异面板（未写入任何内容）"
                                   : "External change detected; read-only diff opened (nothing written)", 1);
                return;
            }
            envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
            backups.saveEnv(liveUser.snap, sysW);
            const std::string ref = currentRowRef();
            const auto d = m_env.delta(sysW);
            int ok = 0;
            std::vector<std::string> failed;
            for (const auto& r : d.toWrite) {
                if (store.writeVar(r.name, r.value, r.type, r.scope))
                    ++ok;
                else
                    failed.push_back(r.scope + "/" + r.name);
            }
            for (const auto& del : d.toDelete) {
                if (store.deleteVar(del.name, del.scope))
                    ++ok;
                else
                    failed.push_back(del.scope + "/" + del.name);
            }
            store.broadcastSettingsChange();
            // Re-read with the scope this window shows. A user-only snapshot used
            // to be installed as the new base, which erased every system row.
            m_env = envh::edit::EnvModel::fromSnapshot(store.read(m_args.showSystem).snap);
            m_extChanged = false;
            m_extFingerprint.clear();
            m_diff = envh::edit::Diff3Result {};
            clearDraft();
            const std::string warnSuffix = warns.empty()
                ? std::string {}
                : (m_args.zh ? "（⚠ 含超长值，新进程可能读不到完整内容）"
                             : " (⚠ over-length value; new processes may not read it back)");
            if (failed.empty())
                setStatus((m_args.zh ? "已保存 " : "Saved ") + std::to_string(ok)
                              + (m_args.zh ? " 项" : "") + warnSuffix, warns.empty() ? 3 : 1);
            else
                setStatus((m_args.zh ? "部分写入失败：" : "Partial write failed: ") + join(failed)
                              + (m_args.zh ? "（已写入 " + std::to_string(ok) + " 项）"
                                           : " (" + std::to_string(ok) + " written)"),
                          2);
            if (ref.empty() || !selectByRef(ref))
                selectRow(m_sel);
        } else {
            envh::native::HostsStore store(m_args.hostsPath);
            const envh::native::HostsReadResult rr = store.read();
            if (!rr.ok) {
                setStatus(m_args.zh ? "读取失败，无法保存" : "Read failed; cannot save", 2);
                return;
            }
            if (!force && envh::edit::HostsModel::contentFingerprint(rr.content) != m_hosts.fingerprint) {
                m_extFingerprint = envh::edit::HostsModel::contentFingerprint(rr.content);
                m_diffHosts = envh::edit::diff3Hosts(envh::serializeHosts(m_hosts.disk), rr.content);
                m_extChanged = true;
                m_extSeenTick = GetTickCount64();
                openDiff();
                InvalidateRect(m_hwnd, nullptr, TRUE);
                setStatus(m_args.zh ? "hosts 已被外部修改，已打开只读差异面板（未写入任何内容）"
                                   : "hosts changed externally; read-only diff opened (nothing written)", 1);
                return;
            }
            envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
            backups.saveHosts(rr.content);
            std::string err;
            if (!store.write(m_hosts.serializeWork(), err)) {
                setStatus(m_args.zh ? ("写入失败：" + err) : ("Write failed: " + err), 2);
                return;
            }
            const envh::HostsParsed p = envh::parseHosts(rr.content);
            m_hosts = envh::edit::HostsModel::fromParsed(p, dominantCrlf(p));
            m_extChanged = false;
            m_extFingerprint.clear();
            m_diffHosts = envh::edit::Diff3HostsResult {};
            clearDraft();
            setStatus(m_args.zh ? "已保存 hosts" : "Saved hosts", 3);
        }
        refreshBackups();
        rebuildVisible();
        selectRow(m_sel);
    }

    void setStatus(const std::string& s, int kind = 0)
    {
        winshun::debug("ra9fael.envhosts: " + s);
        m_statusFlash = s;
        m_statusFlashKind = kind;
        m_statusFlashUntil = GetTickCount64() + 4000;
        if (m_hwnd) {
            SetTimer(m_hwnd, kTimerStatus, 4000, nullptr);
            InvalidateRect(m_hwnd, nullptr, TRUE);
        }
    }
    std::string m_statusFlash;

    void refreshBackups()
    {
        m_backupIds.clear();
        m_backupCounts.clear();
        m_backupHasUser.clear();
        m_backupHasSystem.clear();
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        if (m_kind == "env") {
            const auto all = backups.listEnv();
            m_backupTotal = all.size();
            for (const auto& b : all) {
                // Row count and scope presence per backup, cached: reading the
                // snapshot in the paint path would hit the disk on every
                // repaint. The backup JSON stores the scope of every var.
                const envh::EnvSnapshot snap = backups.readEnv(b.id);
                int user = 0, system = 0;
                for (const auto& v : snap.vars) {
                    if (v.scope == "system")
                        ++system;
                    else
                        ++user;
                }
                // A backup with neither scope carries nothing useful for the
                // current filter, so it is skipped entirely.
                if (m_backupScope == kScopeUser && user == 0)
                    continue;
                if (m_backupScope == kScopeSystem && system == 0)
                    continue;
                m_backupIds.push_back(b.id);
                m_backupCounts.push_back(static_cast<int>(snap.vars.size()));
                m_backupHasUser.push_back(user > 0 ? 1 : 0);
                m_backupHasSystem.push_back(system > 0 ? 1 : 0);
            }
        } else {
            const auto all = backups.listHosts();
            m_backupTotal = all.size();
            for (const auto& b : all) {
                m_backupIds.push_back(b.id);
                const std::string content = backups.readHosts(b.id);
                int n = 0;
                for (const char c : content)
                    if (c == '\n')
                        ++n;
                m_backupCounts.push_back(n);
                // hosts backups have no registry scope; keep them visible in
                // every scope mode.
                m_backupHasUser.push_back(1);
                m_backupHasSystem.push_back(0);
            }
        }
    }

    void loadBackupIntoWork(const std::string& id)
    {
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        if (m_kind == "env") {
            envh::EnvSnapshot snap;std::set<std::string> scopes;std::string error;
            if (!backups.readEnvChecked(id, snap, scopes, error)) {
                setStatus(error + (m_args.zh ? "：请使用生成此备份的版本恢复" : ": restore with the edition that created this backup"), 2);
                return;
            }
            clearPartSessions();
            // The backup is the target state for the scopes it holds; scopes it
            // does not hold (system, in a user-only backup) come from a live
            // read, so loading it never marks them deleted.
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            const envh::EnvSnapshot live = store.read(m_args.showSystem).snap;
            for (const envh::EnvVar& v : live.vars)
                if (!scopes.count(v.scope))
                    snap.vars.push_back(v);
            const envh::edit::EnvModel liveModel = envh::edit::EnvModel::fromSnapshot(live);
            m_env = liveModel;
            // Turn the backup into edits on top of the live base: dirty vs disk.
            std::vector<envh::edit::EnvVarRow> rows;
            for (const envh::EnvVar& v : snap.vars) {
                envh::edit::EnvVarRow r;
                r.scope = v.scope;
                r.name = v.name;
                r.type = v.type;
                r.value = v.value;
                r.inBase = false;
                for (const auto& b : liveModel.base)
                    if (b.scope == r.scope && envh::toLower(b.name) == envh::toLower(r.name))
                        r.inBase = true;
                r.isNew = !r.inBase;
                rows.push_back(r);
            }
            for (const auto& b : liveModel.base) {
                bool kept = false;
                for (const auto& r : rows)
                    if (r.scope == b.scope && envh::toLower(r.name) == envh::toLower(b.name))
                        kept = true;
                if (!kept) {
                    envh::edit::EnvVarRow r = b;
                    r.deleted = true;
                    rows.push_back(r);
                }
            }
            m_env.pushUndo();
            m_env.rows = std::move(rows);
        } else {
            const std::string path = m_args.dataDir + "/backups/hosts/" + id + ".hosts";
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return;
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const envh::HostsParsed p = envh::parseHosts(content);
            m_hosts = envh::edit::HostsModel::fromParsed(p, dominantCrlf(p));
        }
        switchTab(0);
        setStatus(m_args.zh ? ("已载入备份 " + id + "（未保存）") : ("Loaded " + id + " (unsaved)"));
    }

    // ---- row commands (shared by keyboard, buttons and status chips) --------
    int findVisible(int modelRow) const
    {
        for (int v = 0; v < static_cast<int>(m_visible.size()); ++v)
            if (m_visible[v] == modelRow)
                return v;
        return -1;
    }
    bool currentRowDeleted() const
    {
        if (m_visible.empty() || m_tab != 0)
            return false;
        return m_kind == "env" ? m_env.rows[m_visible[m_sel]].deleted
                               : m_hosts.rows[m_visible[m_sel]].deleted;
    }
    bool canDeleteRow() const { return canDeleteSelection(); }
    void clearFilter()
    {
        if (m_filterText.empty())
            return;
        m_filterText.clear();
        m_loading = true;
        if (m_filterEdit)
            SetWindowTextW(m_filterEdit, L"");
        m_loading = false;
    }
    void doAdd()
    {
        endEdit();
        if (!canAddRow()) {
            setStatus(m_args.zh ? "当前分组不可新增" : "Cannot add in this view", 2);
            return;
        }
        clearFilter(); // a new empty row would not match an active filter
        int idx = -1;
        if (m_kind == "env") {
            idx = static_cast<int>(m_env.addNew(m_tab == 1 ? "system" : "user"));
        } else {
            const int after = !m_visible.empty() ? m_hosts.rows[m_visible[m_sel]].origin : -1;
            idx = static_cast<int>(m_hosts.addNew(after));
        }
        rebuildVisible();
        clearSelection();
        const int v = findVisible(idx);
        selectRow(v < 0 ? 0 : v);
        SetFocus(m_kind == "env" ? m_nameEdit : m_ipEdit);
        setStatus(m_args.zh ? "已新增一行（未保存）" : "Row added (unsaved)", 3);
    }
    // Deleting a base variable removes the ENTIRE value, including every split
    // item. In item mode this used to happen silently while the items stayed on
    // screen, so saving looked like "Path disappeared". Ask once, by name.
    bool confirmWholeRowDelete(const std::vector<int>& idx) const
    {
        if (m_kind != "env" || m_tab == 2 || idx.empty())
            return true;
        int base = 0;
        for (int i : idx)
            if (m_env.rows[i].inBase && !m_env.rows[i].deleted)
                ++base;
        if (base == 0)
            return true; // new rows are simply removed; restores need no warning
        std::string names;
        for (std::size_t k = 0; k < idx.size() && k < 5; ++k)
            names += (k ? ", " : "") + m_env.rows[idx[k]].name;
        if (idx.size() > 5)
            names += " …";
        std::string items;
        if (m_listMode && idx.size() == 1) {
            const auto& r = m_env.rows[idx[0]];
            items = std::to_string(envh::edit::splitDelimited(
                        r.value, preferredDelimiter(r.name, r.value), true).size());
        }
        const std::string what = names + (items.empty() ? "" : " (" + items + (m_args.zh ? " 个分隔项)" : " split items)"));
        envh::dlg::Spec ds;
        ds.title = m_args.zh ? L"标记删除整个变量" : L"Mark whole variable deleted";
        ds.subtitle = wideFromUtf8(what);
        ds.body = wideFromUtf8(m_args.zh
            ? "即将标记删除整个变量 " + what + "。\n\n该变量的全部分隔项修改也会一起丢弃。\n\n提交修改（Ctrl+S）后会从注册表移除该变量。"
            : "This marks the whole variable " + what + " for deletion.\n\nAny unsaved split-item edits are discarded with it.\n\nCommitting (Ctrl+S) removes it from the registry.");
        ds.icon = envh::dlg::kDanger;
        ds.buttons = {
            {m_args.zh ? L"取消" : L"Cancel", IDNO, envh::dlg::kPlain},
            {m_args.zh ? L"标记删除" : L"Mark deleted", IDYES, envh::dlg::kDangerBtn},
        };
        ds.defButton = 0;
        return envh::dlg::show(m_hwnd, ds, th(), m_dark) == IDYES;
    }
    // Split the selection into rows this window is allowed to write.
    std::vector<int> writableSelection(bool forDelete, int& skipped) const
    {
        std::vector<int> out;
        skipped = 0;
        for (int i : selectedModelIndices()) {
            bool ok = false;
            if (m_kind == "env") {
                const auto& r = m_env.rows[i];
                ok = r.scope == "user" || (r.scope == "system" && m_args.systemWritable);
            } else {
                const auto& r = m_hosts.rows[i];
                ok = m_tab == 0 && m_args.writable && !r.isSection;
                if (forDelete == false)
                    ok = ok && !r.deleted && !r.isNew; // enable/disable only
            }
            if (ok)
                out.push_back(i);
            else
                ++skipped;
        }
        return out;
    }
    bool canDeleteSelection() const
    {
        if (m_tab == 2 || (m_kind == "hosts" && m_tab == 1) || m_visible.empty())
            return false;
        int skipped = 0;
        return !writableSelection(true, skipped).empty();
    }
    void doDelete()
    {
        endEdit();
        if (!canDeleteSelection())
            return;
        int skipped = 0;
        const std::vector<int> idx = writableSelection(true, skipped);
        if (!confirmWholeRowDelete(idx))
            return;
        bool allDeleted = true, allNew = true;
        for (int i : idx) {
            const bool d = m_kind == "env" ? m_env.rows[i].deleted : m_hosts.rows[i].deleted;
            const bool n = m_kind == "env" ? !m_env.rows[i].inBase : m_hosts.rows[i].origin < 0;
            if (!d) allDeleted = false;
            if (!n) allNew = false;
        }
        const bool restoring = allDeleted && !allNew;
        const bool target = !restoring;
        std::vector<std::size_t> wide(idx.begin(), idx.end());
        if (m_kind == "env") {
            if (target) {
                for (int i : idx)
                    m_partSessions.erase(m_env.rows[i].scope + "/" + m_env.rows[i].name);
                m_partRef.clear();
            }
            m_env.setDeletedMany(wide, target);
        } else
            m_hosts.setDeletedMany(wide, target);
        m_skippedSelected = skipped;
        rebuildVisible();
        clearSelection();
        selectRow(m_sel);
        const std::string n = std::to_string(idx.size());
        setStatus(allNew ? (m_args.zh ? "已移除新增行 " + n : "New rows removed: " + n)
                  : restoring ? (m_args.zh ? "已恢复 " + n + " 行" : n + " rows restored")
                              : (m_args.zh ? "已标记删除 " + n + " 行（再按 Del 恢复）"
                                           : n + " rows marked deleted (Del restores)"), 3);
    }
    void doToggleEnabled()
    {
        endEdit();
        if (m_kind != "hosts" || m_tab != 0 || !m_args.writable)
            return;
        int skipped = 0;
        const std::vector<int> idx = writableSelection(false, skipped);
        if (idx.empty())
            return;
        bool allEnabled = true;
        for (int i : idx)
            if (m_hosts.rows[i].commented)
                allEnabled = false;
        std::vector<std::size_t> wide(idx.begin(), idx.end());
        m_hosts.setEnabledMany(wide, !allEnabled ? true : false);
        m_skippedSelected = skipped;
        rebuildVisible();
        clearSelection();
        selectRow(m_sel);
        setStatus(m_args.zh ? (allEnabled ? "已禁用所选行" : "已启用所选行")
                            : (allEnabled ? "Selected rows disabled" : "Selected rows enabled"), 3);
    }
    void doCopySelection()
    {
        if (m_tab == 2 || (m_kind == "hosts" && m_tab == 1))
            return;
        std::string text;
        for (int i : selectedModelIndices()) {
            if (!text.empty())
                text += "\r\n";
            if (m_kind == "env") {
                const auto& r = m_env.rows[i];
                text += r.name + "=" + r.value;
            } else {
                const auto& r = m_hosts.rows[i];
                if (r.isSection) {
                    text += r.sectionText;
                    continue;
                }
                text += (r.commented ? std::string("# ") : std::string()) + r.ip + " " + join(r.hostnames);
                if (!r.comment.empty())
                    text += " # " + r.comment;
            }
        }
        if (text.empty() || !OpenClipboard(m_hwnd))
            return;
        const std::wstring w = wideFromUtf8(text);
        const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            if (void* p = GlobalLock(h)) {
                memcpy(p, w.c_str(), bytes);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
        setStatus(m_args.zh ? "已复制所选行" : "Selected rows copied", 3);
    }
    void doUndo()
    {
        endEdit();
        if (m_kind == "env" && undoPartChange(false))
            return;
        m_partRef.clear();
        if (m_kind == "hosts" && m_tab == 1) {
            SendMessageW(m_rawEdit, EM_UNDO, 0, 0); // text-level undo inside the raw box
            return;
        }
        if (m_kind == "env")
            m_env.undo();
        else
            m_hosts.undo();
        clearSelection(); // undo restores whole row vectors; old indices are stale
        m_skippedSelected = 0;
        rebuildVisible();
        selectRow(m_sel);
    }
    void doRedo()
    {
        endEdit();
        if (m_kind == "env" && undoPartChange(true))
            return;
        m_partRef.clear();
        if (m_kind == "env")
            m_env.redo();
        else
            m_hosts.redo();
        clearSelection();
        m_skippedSelected = 0;
        rebuildVisible();
        selectRow(m_sel);
    }
    void doEdit()
    {
        endEdit();
        if (!rowEditable(m_sel))
            return;
        if (multiSelectActive())
            setStatus(m_args.zh ? "多行已选，仅编辑焦点行；Esc 取消多选" : "Rows selected; editing the focus row only (Esc clears)", 1);
        SetFocus(m_kind == "hosts" ? m_ipEdit : (m_listMode && m_partsList ? m_partsList : m_valueEdit));
    }
    void runChip(int id)
    {
        switch (id) {
        case kChipAdd: doAdd(); break;
        case kChipDel: doDelete(); break;
        case kChipEdit: doEdit(); break;
        case kChipUndo: doUndo(); break;
        case kChipRedo: doRedo(); break;
        case kChipSave: save(); break;
        case kChipClose: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
        case kChipPartUp: movePart(-1); break;
        case kChipPartDown: movePart(1); break;
        }
    }
    void switchTab(int i)
    {
        if (m_tab == i)
            return;
        stashPartView();
        m_lastG = m_lastD = 0;
        m_vimCount = -1;
        // Leaving the raw tab applies its text to the model (like any other
        // edit in this window, nothing reaches the file before Ctrl+S).
        if (m_kind == "hosts" && m_tab == 1)
            commitRaw();
        m_topRowByTab[m_tab] = m_topRow;
        m_tab = i;
        clearSelection(); // selection is a per-tab view state
        m_skippedSelected = 0;
        if (i == 2)
            refreshBackups();
        m_topRow = m_topRowByTab[i];
        rebuildVisible();
        RECT cr {};
        GetClientRect(m_hwnd, &cr);
        computeLayout(cr.right, cr.bottom); // the raw tab has no detail strip
        clampScroll();
        selectRow(m_topRow);
        if (m_kind == "hosts" && i == 1)
            loadRaw();
        applyLayout();
        if (m_kind == "hosts" && i == 1 && m_rawEdit)
            SetFocus(m_rawEdit); // wheel / PgUp / PgDn go to the text box
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    // ---- hosts raw tab ----
    bool rawEditable() const
    {
        return m_kind == "hosts" && m_hasHosts && m_args.writable && m_errorBanner().empty();
    }
    // Fill the raw box with what Ctrl+S would write now. A multi-line EDIT
    // only breaks lines on CRLF, so LF is widened here and narrowed again in
    // HostsModel::applyRaw.
    void loadRaw()
    {
        if (!m_rawEdit)
            return;
        std::string text = m_hasHosts ? m_hosts.serializeWork() : std::string {};
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
            && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);
        std::string crlf;
        crlf.reserve(text.size() + text.size() / 16);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
                crlf += '\r';
            crlf += text[i];
        }
        m_loading = true;
        SetWindowTextW(m_rawEdit, wideFromUtf8(crlf).c_str());
        SendMessageW(m_rawEdit, EM_EMPTYUNDOBUFFER, 0, 0);
        m_loading = false;
        m_rawShown = getEdit(m_rawEdit);
    }
    bool rawDirty() const
    {
        return m_kind == "hosts" && m_tab == 1 && m_rawEdit && rawEditable() && getEdit(m_rawEdit) != m_rawShown;
    }
    // Parse the raw text back into the model. One raw edit = one undo step.
    void commitRaw()
    {
        if (!rawDirty())
            return;
        const std::string text = getEdit(m_rawEdit);
        if (m_hosts.applyRaw(text)) {
            rebuildVisible();
            setStatus(m_args.zh ? "已应用原文修改（未保存）" : "Raw edit applied (unsaved)", 3);
        }
        m_rawShown = text;
    }
    // 1-based caret line and total line count of the raw box.
    std::pair<int, int> rawCaretLine() const
    {
        if (!m_rawEdit)
            return {0, 0};
        const int line = static_cast<int>(SendMessageW(m_rawEdit, EM_LINEFROMCHAR, static_cast<WPARAM>(-1), 0)) + 1;
        int total = static_cast<int>(SendMessageW(m_rawEdit, EM_GETLINECOUNT, 0, 0));
        // EDIT counts the empty line after a final newline; the file does not.
        const std::string t = getEdit(m_rawEdit);
        if (total > 1 && !t.empty() && t.back() == '\n')
            --total;
        return {std::min(line, total), total};
    }

    // ---- wndproc ----
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wp == kTimerDraft) {
                persistDraftIfDirty();
            } else if (wp == kTimerCommit) {
                endEdit();
                InvalidateRect(m_hwnd, nullptr, TRUE);
            } else if (wp == kTimerStatus) {
                m_statusFlash.clear();
                KillTimer(m_hwnd, kTimerStatus);
                InvalidateRect(m_hwnd, nullptr, TRUE);
            } else if (wp == kTimerWatch) {
                watchExternal();
            }
            return 0;
        case WM_VSCROLL: {
            const int code = LOWORD(wp);
            int top = m_topRow;
            switch (code) {
            case SB_LINEUP: --top; break;
            case SB_LINEDOWN: ++top; break;
            case SB_PAGEUP: top -= pageRows(); break;
            case SB_PAGEDOWN: top += pageRows(); break;
            case SB_TOP: top = 0; break;
            case SB_BOTTOM: top = maxTopRow(); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si {};
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                if (GetScrollInfo(m_hwnd, SB_VERT, &si))
                    top = si.nTrackPos;
                break;
            }
            }
            scrollTo(top);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0,
                                  reinterpret_cast<PVOID>(&lines), 0);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = static_cast<int>(lines) * delta / WHEEL_DELTA;
            if (step == 0)
                step = delta > 0 ? -1 : 1;
            scrollTo(m_topRow - step);
            return 0;
        }
        case WM_APP_FOCUS:
            if (lp)
                applyFocus(reinterpret_cast<OpenArgs*>(lp));
            return 0;
        case WM_APP_EXTCHANGE:
            externalChange();
            return 0;
        case WM_APP_APPEARANCE: {
            std::unique_ptr<AppearanceArgs> a(reinterpret_cast<AppearanceArgs*>(lp));
            if (a)
                applyAppearance(*a);
            return 0;
        }
        case WM_APP_KEY:
            return onKey(wp, static_cast<int>(lp));
        case WM_APP_GETSTATE: {
            int deleted = 0, commented = 0;
            if (m_kind == "env") {
                for (const auto& r : m_env.rows)
                    if (r.deleted)
                        ++deleted;
            } else {
                for (const auto& r : m_hosts.rows) {
                    if (r.deleted)
                        ++deleted;
                    if (!r.isSection && r.commented)
                        ++commented;
                }
            }
            // v0.7.1: bits 3-7 of the flags byte carry the staged item-delete
            // count (0..31); the GUI suite asserts the two-stage flow.
            // Bits 56-63 carry the caret row (visible index, clamped to 255)
            // so the suite can assert vim-style j/k/gg/G navigation.
            const int staged = stagedPartCount() & 0x1f;
            const int flags = (m_diffOpen ? 1 : 0) | (m_extChanged ? 2 : 0)
                              | (static_cast<int>(envWarnings().size()) > 0 ? 4 : 0)
                              | (staged << 3);
            return static_cast<LRESULT>(selectionCount() & 0xffff)
                | (static_cast<LRESULT>(deleted & 0xffff) << 16)
                | (static_cast<LRESULT>(commented & 0xffff) << 32)
                | (static_cast<LRESULT>(flags & 0xff) << 48)
                | (static_cast<LRESULT>(std::max(0, m_sel) & 0xff) << 56);
        }
        case WM_APP_GETLAYOUT: {
            // v0.7.2 geometry probe. wParam selects the rect, lParam the
            // notice-band index for the band queries.
            auto pack = [](const RECT& r) -> LRESULT {
                auto q = [](int v) -> LRESULT {
                    return static_cast<LRESULT>(static_cast<UINT>(v) & 0xffffu);
                };
                return q(r.left) | (q(r.top) << 16) | (q(r.right) << 32) | (q(r.bottom) << 48);
            };
            switch (wp) {
            case 0: return pack(m_rcDiffPanel);
            case 1: return pack(m_rcDiffFooter);
            case 2: return pack(m_rcDiffReload);
            case 3: return pack(m_rcItemHint);
            case 4: return pack(m_rcCommitBtn);
            case 5: return pack(m_rcApply);
            case 6: return pack(m_rcDelimiter);
            case 7: return pack(m_rcModeRaw);
            case 8: return pack(m_rcModeList);
            case 12: return pack(m_rcPartUp);
            case 13: return pack(m_rcPartDown);
            case 14: return pack(m_rcStatus);
            case 15: return lp >= 0 && static_cast<std::size_t>(lp) < m_chips.size()
                ? pack(m_chips[lp].rc) : 0;
            case 16: {
                if (lp < 0 || static_cast<std::size_t>(lp) >= m_chips.size()) return 0;
                HDC dc = GetDC(m_hwnd);
                HFONT font = makeFont(11, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
                HGDIOBJ old = SelectObject(dc, font);
                const wchar_t* label = chipLabel(m_chips[lp].id);
                SIZE size {};
                GetTextExtentPoint32W(dc, label, lstrlenW(label), &size);
                SelectObject(dc, old);
                DeleteObject(font);
                ReleaseDC(m_hwnd, dc);
                return pack(RECT {0, 0, size.cx, size.cy});
            }
            case 17: return static_cast<LRESULT>(m_backupTotal);
            case 18: return static_cast<LRESULT>(m_backupIds.size());
            case 19: return m_env.isDirty();
            case 20: return pendingPartChanges();
            case 21: return lp >= 0 && lp < 3 ? pack(m_rcBackupScope[lp]) : 0;
            case 22: return m_kind == "env" && !m_visible.empty()
                ? static_cast<LRESULT>(m_env.rows[m_visible[m_sel]].value.size()) : 0;
            case 23: return pack(m_rcDetail);
            case 24: return pack(m_rcSplitter);
            case 25: {
                if (m_kind != "env" || m_tab == 2 || m_visible.empty()) return 0;
                const auto index = static_cast<std::size_t>(m_visible[m_sel]);
                const auto state = m_env.rowState(index);
                return (envRowPending(index) ? 1 : 0)
                    | (state != envh::edit::RowState::Unchanged ? 2 : 0)
                    | (state == envh::edit::RowState::Added ? 4 : 0)
                    | (state == envh::edit::RowState::Deleted ? 8 : 0);
            }
            case 26: { // Actual row-label dimensions with the painting font.
                if (m_kind != "env" || m_tab == 2 || m_visible.empty()) return 0;
                const auto index = static_cast<std::size_t>(m_visible[m_sel]);
                const wchar_t* label = envStateLabel(m_env.rowState(index), envRowPending(index));
                HDC dc = GetDC(m_hwnd);
                HFONT font = makeFont(11, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
                HGDIOBJ old = SelectObject(dc, font);
                SIZE size {};
                GetTextExtentPoint32W(dc, label, lstrlenW(label), &size);
                SelectObject(dc, old);
                DeleteObject(font);
                ReleaseDC(m_hwnd, dc);
                return pack(RECT {0, 0, size.cx, size.cy});
            }
            case 27: return pack(RECT {m_colX[3], 0, m_colX[4], m_rowH});
            case 28: return static_cast<LRESULT>(m_chips.size());
            case 29: return (chipEnabled(kChipPartUp) ? 1 : 0) | (chipEnabled(kChipPartDown) ? 2 : 0);
            case 30: return pack(m_rcPartGroup);
            case 9: {
                // Notice band i: left | top<<16 | right<<32 | bottom<<48.
                const int i = static_cast<int>(lp);
                const int top = m_rcTable.top + m_headerH + i * kBandH;
                return pack(RECT {m_rcTable.left, top, m_rcTable.right, top + kBandH});
            }
            case 10: { // notice band i foreground (COLORREF)
                const auto ns = notices();
                const int i = static_cast<int>(lp);
                if (i < 0 || i >= static_cast<int>(ns.size()))
                    return 0;
                return static_cast<LRESULT>(ns[i].fg);
            }
            case 11: { // notice band i background (COLORREF)
                const auto ns = notices();
                const int i = static_cast<int>(lp);
                if (i < 0 || i >= static_cast<int>(ns.size()))
                    return 0;
                return static_cast<LRESULT>(ns[i].bg);
            }
            default: return 0;
            }
        }
        // Native child controls (EDIT / LISTBOX / COMBOBOX / BUTTON) erase and
        // paint with the system colors, which stay light in the dark theme and
        // make the detail fields look like holes cut in the panel.
        case WM_DRAWITEM: {
            // v0.6.0 item table: one row = # | item | chars | action.
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!di || di->hwndItem != m_partsList || di->itemID == static_cast<UINT>(-1))
                return 0;
            HFONT ui = makeFont(13, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
            HFONT small = makeFont(11, false, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
            const COLORREF fg = th().kText;
            const COLORREF dim = th().kTextDim2;
            drawPartRow(*di, small, ui, fg, dim);
            DeleteObject(ui);
            DeleteObject(small);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            if (!m_dark) {
                // The checkbox sits on the light-grey detail panel; the default
                // brush painted a white/grey block behind "启用".
                if (msg == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lp) == m_enabledChk) {
                    if (!m_lightPanelBrush)
                        m_lightPanelBrush = CreateSolidBrush(th().kPanel);
                    HDC dc = reinterpret_cast<HDC>(wp);
                    SetBkColor(dc, th().kPanel);
                    SetTextColor(dc, th().kText);
                    return reinterpret_cast<LRESULT>(m_lightPanelBrush);
                }
                return DefWindowProcW(m_hwnd, msg, wp, lp);
            }
            HDC dc = reinterpret_cast<HDC>(wp);
            const HWND h = reinterpret_cast<HWND>(lp);
            const bool fieldLike = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || h == m_rawEdit;
            const COLORREF bk = fieldLike ? th().kFieldBg : th().kPanel;
            if (!m_darkFieldBrush)
                m_darkFieldBrush = CreateSolidBrush(th().kFieldBg);
            if (!m_darkBgBrush)
                m_darkBgBrush = CreateSolidBrush(th().kBg);
            SetTextColor(dc, fieldLike ? th().kText : th().kBorder);
            SetBkColor(dc, bk);
            return reinterpret_cast<LRESULT>(fieldLike ? m_darkFieldBrush : m_darkBgBrush);
        }
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && lp == 0 && LOWORD(wp) >= kCmdDelimPreset && LOWORD(wp) <= kCmdDelimCustom) {
                onDelimiterCommand(LOWORD(wp));
                return 0;
            }
            if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == m_delimEdit) {
                InvalidateRect(m_hwnd, &m_rcApply, FALSE); // 应用 enables as you type
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED && reinterpret_cast<HWND>(lp) == m_enabledChk
                && !m_loading && !m_visible.empty() && rowEditable(m_sel)) {
                const int index = m_visible[m_sel];
                m_hosts.setEnabled(index, SendMessageW(m_enabledChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                endEdit();
                rebuildVisible();
                selectRow(m_sel);
                return 0;
            }
            if (HIWORD(wp) == LBN_DBLCLK && reinterpret_cast<HWND>(lp) == m_partsList) {
                const int sel = static_cast<int>(SendMessageW(m_partsList, LB_GETCURSEL, 0, 0));
                beginPartInline(sel);
                return 0;
            }
            if (HIWORD(wp) == LBN_SELCHANGE && reinterpret_cast<HWND>(lp) == m_partsList) {
                if (m_partsGuard > 0)
                    return 0;
                // The list already moved its own caret and scroll position. Read
                // the new index first: commitPartEdit() re-asserts the previous
                // selection, which would otherwise snap the list back and make a
                // click look like a jump to the top.
                const int sel = static_cast<int>(SendMessageW(m_partsList, LB_GETCURSEL, 0, 0));
                commitPartEdit();
                if (m_partInline >= 0)
                    endPartInline(true);
                if (sel >= 0 && sel < static_cast<int>(m_valueParts.size()))
                    m_partSel = sel;
                SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
                // v0.7.1: a plain click is a new single selection - it resets
                // any Ctrl/Shift multi-selection made before.
                partSelectionClear();
                if (m_partMulti.size() == m_valueParts.size() && m_partSel >= 0
                    && m_partSel < static_cast<int>(m_valueParts.size()))
                    m_partMulti[static_cast<std::size_t>(m_partSel)] = 1;
                m_partAnchor = m_partSel;
                showCurrentPart();
                invalidatePartChrome();
                InvalidateRect(m_partsList, nullptr, FALSE);
                return 0;
            }
            if (HIWORD(wp) == EN_CHANGE) {
                if (m_loading)
                    return 0;
                if (reinterpret_cast<HWND>(lp) == m_filterEdit) {
                    // Typing in the filter box must actually filter the table.
                    m_filterText = getEdit(m_filterEdit);
                    rebuildVisible();
                    m_topRow = 0;
                    clampScroll();
                    selectRow(0);
                    InvalidateRect(m_hwnd, nullptr, TRUE);
                } else if (reinterpret_cast<HWND>(lp) == m_partEdit) {
                    commitPartEdit();
                } else if (reinterpret_cast<HWND>(lp) == m_rawEdit) {
                    InvalidateRect(m_hwnd, nullptr, FALSE); // dirty marker + Ctrl+S chip
                } else {
                    commitDetail();
                }
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // Keep enough client height for both panes at their minimums
            // (34 top + 112 list + 7 splitter + 200 detail + 48 status).
            auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
            RECT mr {0, 0, 620, 405};
            AdjustWindowRectEx(&mr, windowStyle(), FALSE, 0);
            mmi->ptMinTrackSize.x = mr.right - mr.left;
            mmi->ptMinTrackSize.y = mr.bottom - mr.top;
            return 0;
        }
        case WM_SIZE: {
            RECT cr {};
            GetClientRect(m_hwnd, &cr);
            if (cr.right > 0 && cr.bottom > 0) {
                computeLayout(cr.right, cr.bottom);
                applyLayout();
                InvalidateRect(m_hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int x = static_cast<int>(LOWORD(lp)), y = static_cast<int>(HIWORD(lp));
            const POINT pt {x, y};
            // The diff panel is exclusive: only its three whole-view actions
            // are clickable while it is open.
            if (m_diffOpen) {
                if (inRect(m_rcDiffReload, x, y)) { diffReload(); return 0; }
                if (inRect(m_rcDiffForce, x, y)) { diffForceOverwrite(); return 0; }
                if (inRect(m_rcDiffDefer, x, y)) { diffDefer(); return 0; }
                return 0;
            }
            // Clicking the amber external-change band opens the read-only diff.
            {
                const std::vector<Notice> ns = notices();
                for (std::size_t i = 0; i < ns.size(); ++i) {
                    const int top = m_rcTable.top + m_headerH + static_cast<int>(i) * kBandH;
                    if (ns[i].action == 1 && x >= m_rcTable.left && x < m_rcTable.right
                        && y >= top && y < top + kBandH) {
                        openDiff();
                        return 0;
                    }
                }
            }
            // The scope switch lives in the TAB STRIP (y 6..30), above the
            // work area - so its hit test must run before the
            // "y >= m_rcTable.top" gate below, or the clicks never reach it
            // (that gating was exactly why the switch felt dead).
            if (m_tab == 2 && m_kind == "env") {
                for (int i = 0; i < 3; ++i) {
                    if (inRect(m_rcBackupScope[i], x, y)) {
                        if (m_backupScope != i) {
                            m_backupScope = i;
                            refreshBackups();
                            m_sel = m_backupIds.empty() ? -1 : 0;
                            m_backupPreviewSel = 0;
                            m_topRow = 0;
                            clampScroll();
                            static const char* const zhNames[3] = {"全部", "仅用户", "仅系统"};
                            static const char* const enNames[3] = {"all", "user only", "system only"};
                            setStatus(m_args.zh
                                          ? "备份范围：" + std::string(zhNames[i])
                                          : "Backup scope: " + std::string(enNames[i]),
                                      0);
                            InvalidateRect(m_hwnd, nullptr, TRUE);
                        }
                        return 0;
                    }
                }
            }
            // The backup tab owns the WORK AREA (list + preview + buttons).
            // Clicks in the tab strip above it must fall through to the
            // generic tab handling below, or the user could never leave the
            // backup page (this exact bug made every later check fail).
            if (m_tab == 2 && y >= m_rcTable.top) {
                if (inRect(m_rcBackupPreviewBtn, x, y)) {
                    refreshBackups();
                    m_backupPreviewSel = 0;
                    setStatus(m_args.zh ? "备份预览已刷新" : "Backup preview refreshed", 0);
                    InvalidateRect(m_hwnd, nullptr, TRUE);
                    return 0;
                }
                if (inRect(m_rcBackupRestoreBtn, x, y)) { doBackupRestore(); return 0; }
                if (inRect(m_rcBackupDeleteBtn, x, y)) { doBackupDelete(); return 0; }
                if (inRect(m_rcBackupExportBtn, x, y)) { doBackupExport(); return 0; }
                // Same geometry as the painter: list on the left, preview rows
                // on the right. This used to assume different row origins and
                // every click landed on the wrong entry.
                if (x < m_rcBackupList.right) {
                    const int idx = backupListHit(y);
                    if (idx >= 0) {
                        selectRow(idx);
                        return 0;
                    }
                } else if (x >= m_rcBackupPreview.left && x < m_rcBackupPreview.right) {
                    const int idx = backupPreviewHit(y);
                    if (idx >= 0) {
                        m_backupPreviewSel = idx;
                        InvalidateRect(m_hwnd, nullptr, TRUE);
                        return 0;
                    }
                }
                return 0;
            }
            const int flat = hitFlat(pt);
            if (flat == kFlatModeRaw) {
                setListMode(false);
                return 0;
            }
            if (flat == kFlatModeList) {
                setListMode(true);
                return 0;
            }
            if (flat == kFlatApply) {
                applyDelimiter();
                return 0;
            }
            if (flat == kFlatDelim) {
                showDelimiterMenu();
                return 0;
            }
            if (flat == kFlatPartUp) {
                movePart(-1);
                return 0;
            }
            if (flat == kFlatPartDown) {
                movePart(1);
                return 0;
            }
            if (flat == kFlatPartInsert) {
                addPart();
                return 0;
            }
            if (flat == kFlatPartDelete) {
                deletePart();
                return 0;
            }
            if (flat == kFlatCommit) {
                commitPartChanges();
                return 0;
            }
            if (m_extChanged && inRect(m_rcBandDiff, x, y)) {
                openDiff();
                return 0;
            }
            if (m_extChanged && inRect(m_rcBandReload, x, y)) {
                reloadFromDisk();
                return 0;
            }
            if (m_kind == "env" && m_tab != 2 && inRect(m_rcSplitter, x, y)) {
                m_dragSplitter = true;
                SetCapture(m_hwnd);
                return 0;
            }
            if (inRect(m_rcInfo, x, y)) {
                showPermissionHelp();
                return 0;
            }
            for (Chip c : m_chips)
                if (inRect(c.rc, x, y)) {
                    if (chipEnabled(c.id))
                        runChip(c.id);
                    return 0;
                }
            if (inRect(m_rcAddBtn, x, y)) { doAdd(); return 0; }
            if (inRect(m_rcDelBtn, x, y)) { doDelete(); return 0; }
            if (inRect(m_rcUndoBtn, x, y)) { doUndo(); return 0; }
            if (inRect(m_rcRedoBtn, x, y)) { doRedo(); return 0; }
            for (int i = 0; i < 3; ++i)
                if (inRect(m_rcTabs[i], x, y)) {
                    switchTab(i);
                    return 0;
                }
            // Virtual row hit test: the visible row index is scroll-relative.
            // (The backup tab is fully handled above and never gets here.)
            const bool rawTab = (m_kind == "hosts" && m_tab == 1);
            if (!rawTab && m_tab != 2) {
                const int baseY = m_rcTable.top + m_headerH + bannerH();
                if (y >= baseY && y < m_rcTable.bottom) {
                    const int v = m_topRow + (y - baseY) / m_rowH;
                    if (v >= 0 && v < rowCount()) {
                        // MK_CONTROL / MK_SHIFT also work for injected mouse
                        // messages, where GetAsyncKeyState is not updated.
                        const bool ctrlClick = (wp & MK_CONTROL) != 0
                            || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool shiftClick = (wp & MK_SHIFT) != 0
                            || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        applySelectionGesture(v, ctrlClick, shiftClick);
                        SetFocus(m_hwnd); // the table owns Ins/Del/arrows
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (m_dragSplitter) {
                m_splitterY = static_cast<int>(HIWORD(lp));
                RECT cr {};
                GetClientRect(m_hwnd, &cr);
                computeLayout(cr.right, cr.bottom);
                applyLayout();
                InvalidateRect(m_hwnd, nullptr, TRUE);
                return 0;
            }
            {
                const POINT pt {static_cast<int>(LOWORD(lp)), static_cast<int>(HIWORD(lp))};
                const int old = m_hover;
                m_hover = hitFlat(pt);
                if (old != m_hover) {
                    if (m_hover != 0 || old != 0)
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    TRACKMOUSEEVENT tme {};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = m_hwnd;
                    TrackMouseEvent(&tme);
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            if (m_hover != 0) {
                m_hover = 0;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (m_dragSplitter) {
                m_dragSplitter = false;
                ReleaseCapture();
                return 0;
            }
            return 0;
        case WM_CAPTURECHANGED:
            m_dragSplitter = false;
            return 0;
        case WM_SETCURSOR:
        {
            POINT pt {};
            GetCursorPos(&pt);
            ScreenToClient(m_hwnd, &pt);
            // IDC_* are MAKEINTRESOURCE ids whose pointer type follows UNICODE.
            // LoadCursorW always wants a wide id: cast explicitly so this file
            // builds with or without the macro set.
            if (inRect(m_rcInfo, pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_HAND)));
                return TRUE;
            }
            if (m_kind == "env" && m_tab != 2 && inRect(m_rcSplitter, pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZENS)));
                return TRUE;
            }
            return DefWindowProcW(m_hwnd, msg, wp, lp);
        }
        case WM_KEYDOWN:
            return onKey(wp);
        case WM_CLOSE:
            if (m_kind == "hosts" && m_tab == 1)
                commitRaw();
            if (m_kind == "env" ? m_envDirty() : m_hostsDirty()) {
                envh::dlg::Spec ds;
                ds.title = m_args.zh ? L"关闭" : L"Close";
                ds.subtitle = m_args.zh ? L"有未保存的修改" : L"Unsaved changes";
                ds.body = wideFromUtf8(m_args.zh
                    ? "保存这些修改后再关闭窗口吗？"
                    : "Save the unsaved changes before closing?");
                ds.icon = envh::dlg::kWarn;
                ds.buttons = {
                    {m_args.zh ? L"不保存" : L"Don't save", IDNO, envh::dlg::kPlain},
                    {m_args.zh ? L"取消" : L"Cancel", IDCANCEL, envh::dlg::kPlain},
                    {m_args.zh ? L"保存并关闭" : L"Save and close", IDYES, envh::dlg::kPrimary},
                };
                ds.defButton = 2;
                const int r = envh::dlg::show(m_hwnd, ds, th(), m_dark);
                if (r == IDYES) {
                    save();
                    persistDraftIfDirty();
                } else if (r == IDNO) {
                    persistDraftIfDirty();
                } else {
                    return 0; // cancel
                }
            }
            DestroyWindow(m_hwnd);
            return 0;
        case WM_DESTROY:
            if (m_editFont) {
                DeleteObject(m_editFont);
                m_editFont = nullptr;
            }
            if (m_monoFont) {
                DeleteObject(m_monoFont);
                m_monoFont = nullptr;
            }
            for (HBRUSH* b : {&m_darkBgBrush, &m_darkFieldBrush, &m_lightPanelBrush}) {
                if (*b) {
                    DeleteObject(*b);
                    *b = nullptr;
                }
            }
            if (m_ctx) {
                std::lock_guard<std::mutex> lk(m_ctx->m);
                m_ctx->running = false;
                m_ctx->hwnd = nullptr;
                m_ctx->cv.notify_all();
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(m_hwnd, msg, wp, lp);
        }
    }

    LRESULT onKey(WPARAM wp, int extraMods = 0)
    {
        // extraMods is a test hook (WM_APP_KEY): bit 0 Ctrl, bit 1 Shift,
        // bit 2 Alt (injected messages cannot set the real key state).
        const bool ctrl = (extraMods & 1) != 0 || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
            || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (extraMods & 2) != 0 || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
            || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (extraMods & 4) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool tableFocus = GetFocus() == m_hwnd;
        if ((wp != 'G' && wp != 'g') || ctrl || alt) m_lastG = 0;
        if ((wp != 'D' && wp != 'd') || ctrl || alt) m_lastD = 0;
        // While the diff panel is open it owns the keyboard: no row command,
        // save or tab switch can silently act on a stale view.
        if (m_diffOpen) {
            if (wp == VK_ESCAPE) { diffDefer(); return 0; }
            if (wp == VK_DOWN || wp == VK_NEXT) {
                const int max = m_kind == "env" ? static_cast<int>(m_diff.entries.size())
                                                : static_cast<int>(m_diffHosts.samples.size());
                m_diffScroll = std::max(0, std::min(m_diffScroll + 1, std::max(0, max - 1)));
                InvalidateRect(m_hwnd, nullptr, TRUE);
                return 0;
            }
            if (wp == VK_UP || wp == VK_PRIOR) {
                m_diffScroll = std::max(0, m_diffScroll - 1);
                InvalidateRect(m_hwnd, nullptr, TRUE);
                return 0;
            }
            return 0;
        }
        if (ctrl) {
            if (wp == 'D' || wp == 'd') { openDiff(); return 0; }
            if ((wp == 'E' || wp == 'e') && m_tab == 2) { doBackupExport(); return 0; }
            if (wp == 'S' || wp == 's') { save(); return 0; }
            if (wp == 'Z' || wp == 'z') { doUndo(); return 0; }
            if (wp == 'Y' || wp == 'y') { doRedo(); return 0; }
            if (wp == 'F' || wp == 'f') { if (m_filterEdit) SetFocus(m_filterEdit); return 0; }
            if (wp == 'U' || wp == 'u') {
                // v0.7.1: Ctrl+U marks the whole row(s) deleted (formerly the
                // Del chip only). Falls through to the confirm dialog.
                doDelete();
                return 0;
            }
            // v0.7.1 item-list keys: Ctrl+N add, Ctrl+J/K move the caret
            // without reordering. The floating in-place edit box counts as
            // "the item list owns the keyboard" here: Ctrl+N drops focus into
            // it, and j/k must keep working from there (they close the edit
            // first - see movePartCaret).
            const bool partsNear = GetFocus() == m_partsList || GetFocus() == m_partEdit;
            if (partsNear && m_listMode && m_kind == "env") {
                if (wp == 'N' || wp == 'n') { addPart(); return 0; }
                if (wp == 'J' || wp == 'j') { movePartCaret(1); return 0; }
                if (wp == 'K' || wp == 'k') { movePartCaret(-1); return 0; }
            }
            if (tableFocus && (wp == 'J' || wp == 'K')) {
                selectRow(m_sel + (wp == 'J' ? 1 : -1));
                return 0;
            }
            if (wp == 'R' || wp == 'r') {
                toggleListMode();
                return 0;
            }
            // Ctrl+A / Ctrl+C belong to the table; in an EDIT control they stay
            // native (the subclass does not forward these keys).
            if ((wp == 'A' || wp == 'a') && tableFocus && m_tab != 2
                && !(m_kind == "hosts" && m_tab == 1)) {
                selectAllVisible();
                setStatus(m_args.zh ? "已选中当前视图全部行" : "All rows in this view selected", 0);
                return 0;
            }
            if ((wp == 'C' || wp == 'c') && tableFocus && m_tab != 2
                && !(m_kind == "hosts" && m_tab == 1)) {
                doCopySelection();
                return 0;
            }
            if (wp == VK_TAB) {
                // Ctrl+Tab can arrive while a detail field has focus, and the
                // tab being left may hide that field, so the table keeps focus.
                switchTab((m_tab + (shift ? 2 : 1)) % 3);
                SetFocus(m_hwnd);
                return 0;
            }
            return 0;
        }
        if (m_tab == 2) {
            if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT
                || wp == VK_HOME || wp == VK_END) {
                if (wp == VK_HOME)
                    selectRow(0);
                else if (wp == VK_END)
                    selectRow(rowCount() - 1);
                else {
                    int d = (wp == VK_UP || wp == VK_PRIOR) ? -1 : 1;
                    if (wp == VK_PRIOR || wp == VK_NEXT) {
                        const int p = pageRows();
                        d = (wp == VK_PRIOR ? -p : p);
                    }
                    selectRow(m_sel + d);
                }
                return 0;
            }
            if (wp == VK_RETURN && !m_backupIds.empty()) {
                doBackupRestore();
            }
            if (wp == VK_DELETE && !m_backupIds.empty()) {
                doBackupDelete();
            }
            return 0;
        }
        // alt is declared at the top of onKey (extraMods bit 2 or real key state).
        // Item commands only apply while the split-item list owns the focus.
        // With the table focused, Del is the row command (and now confirms).
        // v0.7.1 keymap: Del/Backspace stage-delete (not while editing),
        // Alt+J/K/Up/Down move the item. Ins stays as a legacy alias.
        // Ctrl+N / Ctrl+J / Ctrl+K are handled in the ctrl branch above.
        const bool partsFocus = GetFocus() == m_partsList || GetFocus() == m_partEdit;
        if (m_kind == "env" && m_listMode && !ctrl && partsFocus) {
            if (wp == VK_INSERT || wp == VK_DELETE || wp == VK_BACK) {
                if (wp == VK_INSERT) { addPart(); return 0; }
                deletePart();
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_F2) {
                // F2 / Enter on an item opens the in-place editor (no separate
                // action pane any more).
                beginPartInline(m_partSel);
                return 0;
            }
        }
        if (m_kind == "env" && m_listMode && partsFocus) {
            if (alt && (wp == 'K' || wp == 'k' || wp == VK_UP)) { movePart(-1); return 0; }
            if (alt && (wp == 'J' || wp == 'j' || wp == VK_DOWN)) { movePart(1); return 0; }
        }
        // Bare j/k move the caret, matching the table vim keys. (Ctrl+J/K are
        // the explicit keymap bindings and land in the ctrl branch above; the
        // floating part EDIT never forwards letters, so typing "j" into an
        // item stays native text input.)
        if (m_kind == "env" && m_listMode && GetFocus() == m_partsList && !alt) {
            if (wp >= '0' && wp <= '9') {
                m_vimCount = std::min(100000, std::max(0, m_vimCount) * 10 + static_cast<int>(wp - '0'));
                return 0;
            }
            const int count = m_vimCount;
            m_vimCount = -1;
            if (wp == 'J') { movePartCaret(std::max(1, count)); return 0; }
            if (wp == 'K') { movePartCaret(-std::max(1, count)); return 0; }
            if (wp == 'G') {
                if (shift || count > 0) {
                    movePartCaret((count > 0 ? count - 1 : static_cast<int>(m_valueParts.size()) - 1) - m_partSel);
                    m_lastG = 0;
                    return 0;
                }
                const ULONGLONG now = GetTickCount64();
                if (m_lastG && now - m_lastG < 600) {
                    movePartCaret(-m_partSel);
                    m_lastG = 0;
                } else {
                    m_lastG = now;
                }
                return 0;
            }
            if (wp == 'D') {
                const ULONGLONG now = GetTickCount64();
                if (m_lastD && now - m_lastD < 600) {
                    deletePart();
                    m_lastD = 0;
                } else {
                    m_lastD = now;
                }
                return 0;
            }
            if (wp == 'X') { deletePart(); return 0; }
            if (wp == 'U') { doUndo(); return 0; }
        }
        // v0.7.1 vim-style navigation on the main table (table focus only, no
        // modifier): j/k one row, gg first row, G last row, x or dd marks the
        // row deleted (the same confirmation dialog as Del), u undoes, / focuses
        // the filter, Enter/l opens the row, h returns from a detail to the
        // table. A digit run is the vim count prefix (3j = three rows down,
        // 5G = row five). Letters typed in a detail EDIT never reach here.
        if (tableFocus && !ctrl && !alt) {
            const auto consumeCount = [&]() {
                const int c = m_vimCount;
                m_vimCount = -1;
                return c;
            };
            if (wp >= '0' && wp <= '9') {
                m_vimCount = std::min(100000, std::max(0, m_vimCount) * 10 + (static_cast<int>(wp) - '0'));
                return 0;
            }
            if (wp == 'J' || wp == 'j') { selectRow(m_sel + std::max(1, consumeCount())); return 0; }
            if (wp == 'K' || wp == 'k') { selectRow(m_sel - std::max(1, consumeCount())); return 0; }
            if (wp == 'G' || wp == 'g') {
                const int c = consumeCount();
                if (c > 0) { // 5G / 5gg: jump to row five
                    m_lastG = 0;
                    selectRow(c - 1);
                    return 0;
                }
                if (shift) { // G: jump to the last row
                    selectRow(rowCount() - 1);
                    return 0;
                }
                const ULONGLONG now = GetTickCount64();
                if (m_lastG && now - m_lastG < 600) { // gg: first row
                    m_lastG = 0;
                    selectRow(0);
                    return 0;
                }
                m_lastG = now;
                return 0;
            }
            if (wp == 'D' || wp == 'd') { // dd: stage-delete, same as x
                consumeCount();
                const ULONGLONG now = GetTickCount64();
                if (m_lastD && now - m_lastD < 600) {
                    m_lastD = 0;
                    doDelete();
                    return 0;
                }
                m_lastD = now;
                return 0;
            }
            if (wp == 'X' || wp == 'x') { consumeCount(); doDelete(); return 0; }
            if (wp == 'U' || wp == 'u') { consumeCount(); doUndo(); return 0; }
            if (wp == 'L' || wp == 'l') { // open the row, same as Enter
                consumeCount();
                if (m_kind == "env")
                    doEdit();
                else
                    doToggleEnabled();
                return 0;
            }
            if (wp == 'H' || wp == 'h') { // back to the table from a detail
                endEdit();
                SetFocus(m_hwnd);
                return 0;
            }
            if (wp == '/' || wp == VK_OEM_2) {
                consumeCount();
                if (m_filterEdit)
                    SetFocus(m_filterEdit);
                return 0;
            }
            m_vimCount = -1; // any other key drops a pending count
        }
        if (wp == VK_ESCAPE) {
            if (multiSelectActive()) {
                clearSelection();
                InvalidateRect(m_hwnd, nullptr, TRUE);
                return 0; // first Esc only drops the selection
            }
            if (m_kind == "env" ? m_envDirty() : m_hostsDirty()) {
                envh::dlg::Spec ds;
                ds.title = m_args.zh ? L"关闭" : L"Close";
                ds.subtitle = m_args.zh ? L"有未保存的修改" : L"Unsaved changes";
                ds.body = wideFromUtf8(m_args.zh
                    ? "保存这些修改后再关闭窗口吗？"
                    : "Save the unsaved changes before closing?");
                ds.icon = envh::dlg::kWarn;
                ds.buttons = {
                    {m_args.zh ? L"不保存" : L"Don't save", IDNO, envh::dlg::kPlain},
                    {m_args.zh ? L"取消" : L"Cancel", IDCANCEL, envh::dlg::kPlain},
                    {m_args.zh ? L"保存并关闭" : L"Save and close", IDYES, envh::dlg::kPrimary},
                };
                ds.defButton = 2;
                const int r = envh::dlg::show(m_hwnd, ds, th(), m_dark);
                if (r == IDYES) { save(); persistDraftIfDirty(); }
                else if (r == IDNO) { persistDraftIfDirty(); }
                else return 0;
            }
            DestroyWindow(m_hwnd);
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT
            || wp == VK_HOME || wp == VK_END) {
            int next = m_sel;
            if (wp == VK_HOME)
                next = 0;
            else if (wp == VK_END)
                next = rowCount() - 1;
            else {
                int d = (wp == VK_UP || wp == VK_PRIOR) ? -1 : 1;
                if (wp == VK_PRIOR || wp == VK_NEXT) {
                    const int p = pageRows();
                    d = wp == VK_PRIOR ? -p : p;
                }
                next = m_sel + d;
            }
            if (shift && tableFocus && !(m_kind == "hosts" && m_tab == 1))
                extendSelectionTo(next);
            else {
                clearSelection();
                selectRow(next);
            }
            return 0;
        }
        if (wp == VK_TAB) {
            switchTab((m_tab + 1) % 3);
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_F2) {
            if (m_kind == "env") doEdit();
            else doToggleEnabled();
            return 0;
        }
        if (wp == VK_SPACE && m_kind == "hosts" && !m_visible.empty()) {
            doToggleEnabled();
            return 0;
        }
        if (wp == VK_INSERT) {
            doAdd();
            return 0;
        }
        if (wp == VK_DELETE) {
            doDelete();
            return 0;
        }
        if (wp == VK_F5) {
            doReload();
            return 0;
        }
        return 0;
    }

    static bool inRect(const RECT& r, int x, int y)
    {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        EditorWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
            self = reinterpret_cast<EditorWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            // The window handle is valid during WM_NCCREATE, but m_hwnd is only
            // assigned from the CreateWindowExW return value afterwards. Set it now
            // so DefWindowProcW (called through onMessage's default branch) sees a
            // valid handle while the window is still being created; otherwise it is
            // called with a null hwnd, returns FALSE, and creation is aborted.
            self->m_hwnd = hwnd;
        } else {
            self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self)
            return self->onMessage(msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

// ---- per-kind thread context (helpers) ---------------------------------------

inline std::map<std::string, ThreadCtx*>& ctxMap()
{
    static std::map<std::string, ThreadCtx*> m;
    return m;
}
inline std::mutex& ctxMutex()
{
    static std::mutex m;
    return m;
}

inline bool openEditor(const OpenArgs& a, std::string& error)
{
    std::lock_guard<std::mutex> lock(ctxMutex());
    auto it = ctxMap().find(a.kind);
    if (it != ctxMap().end() && it->second->running) {
        // already open: refocus with the new args (keep edits)
        std::unique_ptr<OpenArgs> ptr(new OpenArgs(a));
        if (it->second->hwnd)
            PostMessageW(it->second->hwnd, WM_APP_FOCUS, 0, reinterpret_cast<LPARAM>(ptr.release()));
        error.clear();
        return true;
    }
    // Stale entry (window already closed, thread finished): reap before creating a new one.
    if (it != ctxMap().end()) {
        if (it->second->worker.joinable())
            it->second->worker.join();
        delete it->second;
        ctxMap().erase(it);
    }
    ThreadCtx* ctx = new ThreadCtx;
    ctx->running = true;
    ctx->hwnd = nullptr;
    ctx->error.clear();
    ctxMap()[a.kind] = ctx;
    ctx->worker = std::thread([ctx, a]() {
        EditorWindow w(a);
        {
            std::lock_guard<std::mutex> lk(ctx->m);
            ctx->hwnd = nullptr; // set inside run() via CreateWindow; we detect via a flag
        }
        // run() sets hwnd; we need to surface it to the ctx for focus. Patch:
        // EditorWindow stores into ctx through a setter.
        w.setContext(ctx);
        w.run();
        {
            std::lock_guard<std::mutex> lk(ctx->m);
            ctx->running = false;
            ctx->hwnd = nullptr;
            ctx->cv.notify_all();
        }
    });
    // Wait for the window to become visible (or fail).
    std::unique_lock<std::mutex> lk(ctx->m);
    const bool ok = ctx->cv.wait_for(lk, std::chrono::seconds(3), [&] { return ctx->visible || !ctx->error.empty(); });
    if (!ok) {
        error = "timed out waiting for the editor window";
        // leave it running; it will surface its own error
        return false;
    }
    error = ctx->error;
    return ctx->visible;
}

inline bool isEditorOpen(const std::string& kind)
{
    std::lock_guard<std::mutex> lock(ctxMutex());
    auto it = ctxMap().find(kind);
    return it != ctxMap().end() && it->second->running;
}

inline void onExternalChange(const std::string& kind)
{
    std::lock_guard<std::mutex> lock(ctxMutex());
    auto it = ctxMap().find(kind);
    if (it != ctxMap().end() && it->second->hwnd)
        PostMessageW(it->second->hwnd, WM_APP_EXTCHANGE, 0, 0);
}

inline void onAppearanceChanged(envh::i18n::Lang lang, bool dark)
{
    std::lock_guard<std::mutex> lock(ctxMutex());
    for (auto& kv : ctxMap()) {
        ThreadCtx* c = kv.second;
        std::lock_guard<std::mutex> lk(c->m);
        if (c->hwnd)
            PostMessageW(c->hwnd, WM_APP_APPEARANCE, 0,
                         reinterpret_cast<LPARAM>(new AppearanceArgs {lang, dark}));
    }
}

inline void closeAllEditors(unsigned timeoutMs)
{
    std::vector<ThreadCtx*> ctxs;
    {
        std::lock_guard<std::mutex> lock(ctxMutex());
        for (auto& kv : ctxMap())
            ctxs.push_back(kv.second);
    }
    for (ThreadCtx* ctx : ctxs) {
        {
            std::lock_guard<std::mutex> lk(ctx->m);
            if (ctx->hwnd)
                PostMessageW(ctx->hwnd, WM_CLOSE, 0, 0);
        }
        std::unique_lock<std::mutex> lk(ctx->m);
        const bool closed = ctx->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !ctx->running; });
        if (closed && ctx->worker.joinable())
            ctx->worker.join();
    }
}

} // namespace editorwin

#endif // _WIN32
