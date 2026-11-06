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
// Keyboard map (design doc section 7) is implemented: Tab/Ctrl+Tab, Ctrl+F,
// arrows, Enter/F2, Space (hosts toggle), Ins, Del, Alt+Up/Down (PATH segments),
// Ctrl+R (PATH list/raw), Ctrl+C, Ctrl+Z/Y, Ctrl+S, F5, Esc.
//
// Code and comments are English.
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
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

namespace editorwin {

constexpr UINT WM_APP_FOCUS = WM_APP + 1; // apply a new OpenArgs (keep edits)
constexpr UINT WM_APP_EXTCHANGE = WM_APP + 2; // external target change

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
    // writer). Only a change in a scope this window can actually write counts:
    // user scope always; system scope additionally when the plugin runs
    // elevated (systemWritable), matching the save-time conflict check.
    void externalChange()
    {
        if (m_kind == "env") {
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            const bool sysW = m_args.systemWritable;
            const envh::native::EnvReadResult r = store.read(sysW);
            if (!r.ok
                || (sysW ? envh::edit::EnvModel::computeFingerprint(r.snap) == m_env.fingerprint
                         : envh::edit::EnvModel::computeUserFingerprint(r.snap) == m_env.userFingerprint))
                return;
        } else {
            envh::native::HostsStore store(m_args.hostsPath);
            const envh::native::HostsReadResult r = store.read();
            if (!r.ok || envh::edit::HostsModel::contentFingerprint(r.content) == m_hosts.fingerprint)
                return;
        }
        if (isDirty()) {
            m_extChanged = true; // yellow bar; save will surface the conflict
            InvalidateRect(m_hwnd, nullptr, TRUE);
        } else {
            reloadFromDisk();
        }
    }

    void persistDraftIfDirty()
    {
        if (m_kind == "hosts" && m_tab == 1)
            commitRaw(); // a pending raw edit belongs in the draft too
        if (m_kind == "env" && m_envDirty())
            writeDraft(m_env.toDraft());
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
    int m_sel = 0; // index into m_visible
    std::vector<int> m_visible; // indices into the model rows (filtered)
    std::vector<std::string> m_backupIds; // for the backup tab
    std::string m_filterText;
    bool m_listMode = false;
    std::string m_listDelimiter = ";";
    std::map<std::string, std::string> m_delimPrefs; // per-variable applied delimiter
    std::vector<std::string> m_valueParts;
    int m_partSel = 0;
    int m_partsGuard = 0; // >0 while the item list is changed programmatically
    struct PartsGuard {
        EditorWindow* w;
        explicit PartsGuard(EditorWindow* win) : w(win) { ++w->m_partsGuard; }
        ~PartsGuard() { --w->m_partsGuard; }
    };
    int m_splitterY = 0;
    bool m_dragSplitter = false;
    bool m_extChanged = false;
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

    enum ChipId { kChipAdd = 1, kChipDel, kChipEdit, kChipUndo, kChipRedo, kChipSave, kChipClose };
    static constexpr UINT kTimerDraft = 1;
    static constexpr UINT kTimerCommit = 2;
    static constexpr UINT kTimerStatus = 3;
    static constexpr const wchar_t* kClass = L"WinShunEnvHostsEditor";

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
    }

    void loadDraft(const winshun::Json& d, bool markDirty)
    {
        if (m_kind == "env")
            m_env.fromDraft(d);
        else
            m_hosts.fromDraft(d);
        (void)markDirty; // fromDraft makes work differ from base => dirty
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
            n = probe.changeList().size();
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
        const int r = MessageBoxW(m_hwnd, wideFromUtf8(msg).c_str(),
                                  wideFromUtf8(m_args.zh ? "恢复草稿" : "Restore draft").c_str(),
                                  MB_YESNO | MB_ICONQUESTION);
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
        const std::string path = m_args.dataDir + "/drafts/" + m_kind + ".json";
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    void writeDraft(const winshun::Json& d)
    {
        const std::string dir = m_args.dataDir + "/drafts";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        std::ofstream out(std::filesystem::u8path(dir + "/" + m_kind + ".json"), std::ios::binary);
        if (out)
            out.write(d.dump().data(), static_cast<std::streamsize>(d.dump().size()));
    }
    void clearDraft()
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(m_args.dataDir + "/drafts/" + m_kind + ".json"), ec);
    }

    bool m_envDirty() const { return m_hasEnv && m_env.isDirty(); }
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
        clearDraft();
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
            if (MessageBoxW(m_hwnd, wideFromUtf8(body).c_str(),
                            wideFromUtf8(m_args.zh ? "重新载入" : "Reload").c_str(),
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
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
        endEdit(); // changing rows ends the current edit session
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
        if (self && msg == WM_KEYDOWN) {
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
            if ((wp == VK_TAB && ctrl) || wp == VK_INSERT || wp == VK_DELETE || wp == VK_PRIOR
                || wp == VK_NEXT || wp == VK_F5
                || (ctrl && (wp == 'Z' || wp == 'z' || wp == 'Y' || wp == 'y'
                              || wp == 'S' || wp == 's' || wp == 'R' || wp == 'r'))) {
                return self->onKey(wp);
            }
            // Alt+arrows reorder the delimited item under the keyboard focus.
            if (h == self->m_partsList && (GetKeyState(VK_MENU) & 0x8000) != 0
                && (wp == VK_UP || wp == VK_DOWN)) {
                return self->onKey(wp);
            }
        }
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
        m_partsList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", es | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, 0, 0, 10, 20, m_hwnd,
                                     reinterpret_cast<HMENU>(10), GetModuleHandleW(nullptr), nullptr);
        if (m_partsList && m_editFont)
            SendMessageW(m_partsList, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
        // The delimiter is a self-drawn dropdown button (painted like 应用 and
        // the mode segment). This flat EDIT only appears for "自定义…".
        m_delimEdit = CreateWindowExW(0, L"EDIT", L"", es | WS_BORDER | ES_AUTOHSCROLL,
                                      0, 0, 10, 20, m_hwnd,
                                      reinterpret_cast<HMENU>(12), GetModuleHandleW(nullptr), nullptr);
        if (m_delimEdit && m_editFont)
            SendMessageW(m_delimEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
        subclassEdit(m_delimEdit);
        m_partEdit = makeEdit(es | ES_AUTOHSCROLL, 11);
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
        const int statusH = 48;
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
        const int available = std::max(160, static_cast<int>(m_rcDetail.right) - lx - 208);
        const int modeW = std::max(132, std::min(180, available * 30 / 100));
        const int applyW = std::max(54, std::min(72, available * 14 / 100));
        const int comboW = std::max(92, available - modeW - applyW - 12);
        int cx = lx;
        const int cellW = modeW / 2;
        m_rcModeRaw = {cx, modeTop, cx + cellW, modeTop + modeH};
        m_rcModeList = {cx + cellW, modeTop, cx + modeW, modeTop + modeH};
        cx += modeW + 6;
        m_rcDelimiter = {cx, modeTop, cx + comboW, modeTop + modeH};
        cx += comboW + 6;
        m_rcApply = {cx, modeTop, cx + applyW, modeTop + modeH};
        m_rcItemHint = {cx + applyW + 8, modeTop, m_rcDetail.right - 4, modeTop + modeH};
        m_rcStatus = {0, workBottom, m_clientW, m_clientH};
        m_rcStatusRow1 = {0, workBottom, m_clientW, workBottom + 24};
        m_rcStatusRow2 = {0, workBottom + 24, m_clientW, m_clientH};

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
        buildChips();

        // Column boundaries are proportional to the table width so the layout
        // stays correct when the window is resized (WM_SIZE triggers recompute).
        const int L = m_rcTable.left, W = m_rcTable.right - L;
        if (m_kind == "env") {
            m_colCount = 3;
            m_colX[0] = L;
            m_colX[1] = L + W * 30 / 100;
            m_colX[2] = L + W * 46 / 100;
            m_colX[3] = m_rcTable.right;
        } else {
            m_colCount = 5;
            m_colX[0] = L;
            m_colX[1] = L + W * 8 / 100;
            m_colX[2] = L + W * 28 / 100;
            m_colX[3] = L + W * 62 / 100;
            m_colX[4] = m_rcTable.right - W * 10 / 100;
            m_colX[5] = m_rcTable.right;
        }
        clampScroll();
    }

    void buildChips()
    {
        m_chips.clear();
        const int ids[7] = {kChipAdd, kChipDel, kChipEdit, kChipUndo, kChipRedo, kChipSave, kChipClose};
        const int widths[7] = {72, 72, 82, 68, 68, 108, 72};
        const int y = m_rcStatusRow2.top + 4;
        const int h = std::max<int>(14, m_rcStatusRow2.bottom - m_rcStatusRow2.top - 8);
        int x = 8;
        for (int i = 0; i < 7; ++i) {
            if (x + widths[i] > m_clientW - 8)
                break;
            m_chips.push_back({ids[i], {x, y, x + widths[i], y + h}});
            x += widths[i] + 4;
        }
    }

    // ---- virtual list --------------------------------------------------------
    int rowCount() const
    {
        return m_tab == 2 ? static_cast<int>(m_backupIds.size())
                          : static_cast<int>(m_visible.size());
    }
    int pageRows() const
    {
        const int avail = m_rcTable.bottom - m_rcTable.top - (m_tab == 2 ? 4 : m_headerH);
        return std::max(1, avail / m_rowH);
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
            MoveWindow(m_filterEdit, m_rcFilter.left, m_rcFilter.top,
                       m_rcFilter.right - m_rcFilter.left, m_rcFilter.bottom - m_rcFilter.top, TRUE);
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
            ShowWindow(m_partEdit, m_kind == "env" && m_listMode && showDetail && !raw ? SW_SHOW : SW_HIDE);
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
        // The mode/delimiter toolbar is at top+48, labels at top+74 and the
        // value/item area starts at top+90.
        const int lx = m_rcDetail.left + 4;
        const int top = m_rcDetail.top + 20;
        const int valTop = m_rcDetail.top + 90;
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
                const int listW = std::max(180, valueW * 42 / 100);
                const int rightX = lx + listW + 8;
                const int rightW = std::max(150, valueW - listW - 8);
                show(m_partsList, lx, valTop, listW, valueH);
                show(m_partEdit, rightX, valTop, rightW, 24);
                const int buttonY = valTop + 30;
                const int gap = 4;
                const int bw = std::max(48, (rightW - 3 * gap) / 4);
                int bx = rightX;
                m_rcPartUp = {bx, buttonY, bx + bw, buttonY + 22}; bx += bw + gap;
                m_rcPartDown = {bx, buttonY, bx + bw, buttonY + 22}; bx += bw + gap;
                m_rcPartInsert = {bx, buttonY, bx + bw, buttonY + 22}; bx += bw + gap;
                m_rcPartDelete = {bx, buttonY, std::min(rightX + rightW, bx + bw), buttonY + 22};
            } else {
                if (m_partsList)
                    ShowWindow(m_partsList, SW_HIDE);
                if (m_partEdit)
                    ShowWindow(m_partEdit, SW_HIDE);
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
        static std::vector<std::string> kDelims = {";", ",", ":", "\t", "|"};
        return kDelims;
    }
    static std::vector<DelimiterPreset>& delimiterPresetItems()
    {
        static std::vector<DelimiterPreset> kPresets = {
            {";", L"分号 (;)", L"Semicolon (;)"},
            {",", L"逗号 (,)", L"Comma (,)"},
            {":", L"冒号 (:)", L"Colon (:)"},
            {"\t", L"制表符 (Tab)", L"Tab"},
            {"|", L"竖线 (|)", L"Pipe (|)"}};
        return kPresets;
    }
    // Preset menu for the delimiter button. Picking a preset applies it at
    // once (choosing from a menu is already explicit); 自定义… swaps the button
    // for an edit box that is applied with Enter or 应用 and dropped with Esc.
    static constexpr UINT kCmdDelimPreset = 200; // 200..204, then custom
    static constexpr UINT kCmdDelimCustom = 205;
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
        for (const std::string& d : delimiterPresets())
            if (value.find(d) != std::string::npos)
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
        InvalidateRect(m_hwnd, &rc, FALSE);
    }
    // Rebuild only when the delimiter, row, mode or live model changed. A normal
    // selection is handled by updatePartRow/LB_SETCURSEL and must preserve top.
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
        const std::string next = getEdit(m_partEdit);
        if (m_partSel < 0 || m_partSel >= static_cast<int>(m_valueParts.size())
            || m_valueParts[m_partSel] == next)
            return;
        m_valueParts[m_partSel] = next;
        updatePartRow(m_partSel);
        syncRawValueEdit();
        commitDetail();
    }
    // Keep the (hidden) raw value box in sync with the joined list, so toggling
    // back with Ctrl+R and any control-text read see the current value.
    void syncRawValueEdit()
    {
        m_loading = true;
        setEdit(m_valueEdit, envh::edit::joinDelimited(m_valueParts, m_listDelimiter), !rowEditable(m_sel));
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
        m_partSel = insertAt;
        if (m_partsList) {
            const std::wstring empty;
            SendMessageW(m_partsList, LB_INSERTSTRING, insertAt, reinterpret_cast<LPARAM>(empty.c_str()));
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        }
        invalidatePartChrome();
        syncRawValueEdit();
        commitDetail();
        showCurrentPart();
        SetFocus(m_partEdit);
    }
    void deletePart()
    {
        if (!rowEditable(m_sel) || m_valueParts.empty())
            return;
        const PartsGuard g {this};
        commitPartEdit();
        m_valueParts.erase(m_valueParts.begin() + m_partSel);
        if (m_partsList)
            SendMessageW(m_partsList, LB_DELETESTRING, m_partSel, 0);
        if (m_valueParts.empty()) {
            m_valueParts.emplace_back();
            if (m_partsList) {
                const std::wstring empty;
                SendMessageW(m_partsList, LB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(empty.c_str()));
            }
        }
        m_partSel = std::max(0, std::min(m_partSel, static_cast<int>(m_valueParts.size()) - 1));
        if (m_partsList) {
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        }
        invalidatePartChrome();
        syncRawValueEdit();
        commitDetail();
        showCurrentPart();
    }
    void movePart(int delta)
    {
        const int from = m_partSel;
        const int to = from + delta;
        if (!rowEditable(m_sel) || to < 0 || to >= static_cast<int>(m_valueParts.size()))
            return;
        const PartsGuard g {this};
        commitPartEdit();
        std::swap(m_valueParts[from], m_valueParts[to]);
        m_partSel = to;
        updatePartRow(from);
        updatePartRow(to);
        if (m_partsList) {
            SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
            ensurePartVisible();
        }
        invalidatePartChrome();
        syncRawValueEdit();
        commitDetail();
        showCurrentPart();
    }
    // Raw <-> delimited-item editing (Ctrl+R or the mode segment).
    void setListMode(bool enabled)
    {
        if (m_kind != "env" || m_tab == 2 || m_listMode == enabled)
            return;
        commitPartEdit();
        const std::string joined = m_listMode ? envh::edit::joinDelimited(m_valueParts, m_listDelimiter)
                                             : getEdit(m_valueEdit);
        m_listMode = enabled;
        if (m_listMode) {
            const std::string name = m_visible.empty() ? std::string {} : m_env.rows[m_visible[m_sel]].name;
            m_listDelimiter = preferredDelimiter(name, joined);
            m_valueParts = envh::edit::splitDelimited(joined, m_listDelimiter, true);
            m_partSel = 0;
            syncRawValueEdit();
            rebuildPartsList(false);
            syncDelimiterComboText();
            applyLayout();
            SetFocus(m_partsList);
        } else {
            m_valueParts.clear();
            // Write the rejoined value back into the raw box before dropping the
            // parts; syncing from an empty part list would blank the field.
            m_loading = true;
            setEdit(m_valueEdit, joined, !rowEditable(m_sel));
            m_loading = false;
            applyLayout();
            SetFocus(m_valueEdit);
        }
        commitDetail();
        InvalidateRect(m_hwnd, nullptr, TRUE);
        setStatus(m_listMode ? (m_args.zh ? "分隔项编辑 · 分隔符 " + delimiterLabel()
                                          : "item list · delimiter " + delimiterLabel())
                             : (m_args.zh ? "原文编辑" : "raw value editing"),
                  0);
    }
    void toggleListMode() { setListMode(!m_listMode); }

    void applyDelimiter()
    {
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
        commitPartEdit();
        // Re-split from the current full value. If the item box has pending text,
        // commitPartEdit has already written it into this joined value.
        const std::string joined = m_listMode ? envh::edit::joinDelimited(m_valueParts, m_listDelimiter)
                                              : getEdit(m_valueEdit);
        const std::string key = delimiterKey();
        if (!key.empty())
            m_delimPrefs[key] = next;
        m_listDelimiter = next;
        if (m_listMode) {
            m_valueParts = envh::edit::splitDelimited(joined, m_listDelimiter, true);
            m_partSel = 0;
            syncRawValueEdit();
            rebuildPartsList(false);
            syncDelimiterComboText();
            applyLayout();
            InvalidateRect(m_hwnd, nullptr, TRUE);
        }
        commitDetail();
        if (m_delimCustom)
            syncDelimiterComboText();
        m_pendingDelim.clear();
        SetFocus(m_hwnd);
        setStatus(m_args.zh ? "分隔符: " + delimiterLabel() : "delimiter: " + delimiterLabel(), 0);
    }

    // Fill the detail controls from the selected row (no model write).
    void refreshDetail()
    {
        m_loading = true;
        if (m_kind == "env") {
            std::string name, type, value;
            bool ro = false;
            if (!m_visible.empty()) {
                const auto& r = m_env.rows[m_visible[m_sel]];
                name = r.name;
                type = r.type;
                value = r.value;
                ro = (r.scope == "system" && !m_args.systemWritable);
            }
            setEdit(m_nameEdit, name, ro);
            setEdit(m_typeEdit, type, ro);
            setEdit(m_valueEdit, value, ro);
            if (m_listMode) {
                m_listDelimiter = preferredDelimiter(name, value);
                m_valueParts = envh::edit::splitDelimited(value, m_listDelimiter, true);
                m_partSel = 0;
                rebuildPartsList(false);
                syncDelimiterComboText();
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
            const std::string value = m_listMode ? envh::edit::joinDelimited(m_valueParts, m_listDelimiter) : getEdit(m_valueEdit);
            std::string type = getEdit(m_typeEdit);
            if (type.empty())
                type = envh::envTypeForValue(value);
            if (r.name == name && r.value == value && r.type == type)
                return;
            if (!m_editActive) {
                m_env.pushUndo();
                m_editActive = true;
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

        HBRUSH bg = CreateSolidBrush(m_dark ? RGB(24, 26, 30) : GetSysColor(COLOR_WINDOW));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);

        const COLORREF fg = m_dark ? RGB(232, 234, 237) : GetSysColor(COLOR_WINDOWTEXT);
        const COLORREF dim = m_dark ? RGB(120, 128, 136) : RGB(110, 110, 110);
        const COLORREF accent = RGB(88, 166, 255);
        const COLORREF selBg = m_dark ? RGB(38, 42, 48) : RGB(210, 228, 250);
        const COLORREF errFg = RGB(240, 128, 128);

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
        for (int i = 0; i < 3; ++i) {
            const bool active = i == m_tab;
            HBRUSH tb = CreateSolidBrush(active ? accent : (m_dark ? RGB(48, 52, 58) : RGB(228, 232, 238)));
            FillRect(mem, &m_rcTabs[i], tb);
            DeleteObject(tb);
            SetTextColor(mem, active ? RGB(255, 255, 255) : fg);
            DrawTextW(mem, tabs[i].c_str(), -1, &m_rcTabs[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // filter caption (the edit control itself is a real child window)
        {
            RECT fr = m_rcFilterLabel;
            SetTextColor(mem, dim);
            HGDIOBJ of = SelectObject(mem, smallFont);
            DrawTextW(mem, m_args.zh ? L"过滤 Ctrl+F" : L"Filter Ctrl+F", -1, &fr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, of);
            SetTextColor(mem, fg);
        }

        // error banner (hosts read-only is reported in the status line instead)
        if (!m_errorBanner().empty()) {
            RECT b {0, 30, w, 50};
            HBRUSH bb = CreateSolidBrush(RGB(120, 60, 60));
            FillRect(mem, &b, bb);
            DeleteObject(bb);
            SetTextColor(mem, errFg);
            std::wstring msg = wideFromUtf8(m_errorBanner());
            RECT bt = b; bt.left += 10;
            DrawTextW(mem, msg.c_str(), -1, &bt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // ---- table: header band, column separators, rows ----
        if (m_tab != 2) {
            // The column header band was reserved (m_headerH) but never drawn,
            // leaving an unexplained empty strip above the first row.
            HGDIOBJ ohf = SelectObject(mem, smallFont);
            drawHeader(mem, fg, dim);
            SelectObject(mem, ohf);
            HGDIOBJ of = SelectObject(mem, monoFont);
            int y = m_rcTable.top + m_headerH;
            if (m_visible.empty()) {
                SetTextColor(mem, dim);
                RECT er {m_rcTable.left + 12, y + 8, m_rcTable.right - 12, y + 8 + m_rowH};
                const wchar_t* emptyMsg = m_filterText.empty()
                    ? (m_args.zh ? L"（此分组没有条目，按 Ins 新增）" : L"(no entries here; press Ins to add)")
                    : (m_args.zh ? L"（没有匹配的条目）" : L"(no matching entries)");
                DrawTextW(mem, emptyMsg, -1, &er, DT_LEFT | DT_SINGLELINE);
            }
            const int first = std::min(m_topRow, maxTopRow());
            const int last = std::min<int>(rowCount(), first + pageRows());
            for (int v = first; v < last; ++v) {
                const int rowY = m_rcTable.top + m_headerH + (v - first) * m_rowH;
                const RECT row {m_rcTable.left, rowY, m_rcTable.right, rowY + m_rowH};
                HBRUSH rb = CreateSolidBrush(
                    v == m_sel ? selBg
                        : (m_dark ? (v % 2 ? RGB(30, 33, 38) : RGB(24, 26, 30))
                                  : (v % 2 ? RGB(247, 249, 252) : RGB(255, 255, 255))));
                FillRect(mem, &row, rb);
                DeleteObject(rb);
                drawRow(mem, fg, v, row);
                // row separator
                HPEN lp = CreatePen(PS_SOLID, 1, m_dark ? RGB(52, 56, 62) : RGB(230, 233, 238));
                HGDIOBJ op = SelectObject(mem, lp);
                MoveToEx(mem, row.left, row.bottom - 1, nullptr);
                LineTo(mem, row.right, row.bottom - 1);
                SelectObject(mem, op);
                DeleteObject(lp);
            }
            SelectObject(mem, of);
            // table frame
            HPEN fp = CreatePen(PS_SOLID, 1, m_dark ? RGB(70, 76, 84) : RGB(200, 206, 214));
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
            drawBackupList(mem, fg, dim);
            SelectObject(mem, of);
        }

        // ---- detail panel: frame + column labels aligned to the edit controls ----
        if (m_tab != 2 && !(m_kind == "hosts" && m_tab == 1)) {
            HBRUSH db = CreateSolidBrush(m_dark ? RGB(30, 33, 38) : RGB(246, 248, 251));
            FillRect(mem, &m_rcDetail, db);
            DeleteObject(db);
            HPEN dp = CreatePen(PS_SOLID, 1, m_dark ? RGB(70, 76, 84) : RGB(206, 212, 220));
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
                if (currentRowDeleted()) {
                    // A deleted row is read-only until restored; say so instead
                    // of leaving greyed controls unexplained.
                    SetTextColor(mem, m_dark ? RGB(230, 130, 130) : RGB(170, 50, 50));
                    DrawTextW(mem,
                              m_args.zh ? L"已标记删除：保存时会从注册表移除 · 按 Del 恢复后才能编辑"
                                        : L"Marked deleted: removed from the registry on save · press Del to restore and edit",
                              -1, &valueLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SetTextColor(mem, dim);
                } else {
                    DrawTextW(mem,
                              m_listMode ? (m_args.zh ? L"分隔项 · Ins 增加 · Del 删除 · Alt+↑↓ 移动 · Ctrl+R 原文"
                                                      : L"Items · Ins add · Del remove · Alt+Up/Down move · Ctrl+R raw")
                                         : (m_args.zh ? L"原文 · Ctrl+R 分隔项" : L"Raw value · Ctrl+R items"),
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
            HBRUSH sb = CreateSolidBrush(m_dark ? RGB(38, 41, 46) : RGB(236, 239, 244));
            FillRect(mem, &m_rcSplitter, sb);
            DeleteObject(sb);
            HPEN gp = CreatePen(PS_SOLID, 1, m_dark ? RGB(96, 104, 116) : RGB(160, 168, 180));
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
        if (m_kind == "env" && m_tab != 2) {
            drawModeSegment(mem, smallFont, fg, dim);
            const bool enabled = m_listMode && rowEditable(m_sel);
            const bool pending = enabled && delimiterApplyEnabled();
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
            drawFlatButton(mem, smallFont, m_rcApply,
                           m_args.zh ? L"应用" : L"Apply", enabled, pending, 30, fg, dim);
            RECT hint = m_rcItemHint;
            hint.left += 4;
            SetTextColor(mem, dim);
            HGDIOBJ of = SelectObject(mem, smallFont);
            if (m_listMode && !m_valueParts.empty()) {
                const std::wstring text = (m_args.zh ? L"当前项 " : L"Item ")
                    + std::to_wstring(m_partSel + 1) + L" / " + std::to_wstring(m_valueParts.size());
                DrawTextW(mem, text.c_str(), -1, &hint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            } else if (!m_listMode && rowEditable(m_sel) && !pending) {
                DrawTextW(mem, m_args.zh ? L"直接编辑原文" : L"Edit raw value", -1, &hint,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(mem, of);
            SetTextColor(mem, fg);
            if (m_listMode) {
                const bool ed = rowEditable(m_sel);
                const bool empty = m_valueParts.empty();
                drawFlatButton(mem, smallFont, m_rcPartUp, m_args.zh ? L"上移" : L"Up",
                               ed && m_partSel > 0, false, 41, fg, dim);
                drawFlatButton(mem, smallFont, m_rcPartDown, m_args.zh ? L"下移" : L"Down",
                               ed && m_partSel + 1 < static_cast<int>(m_valueParts.size()), false, 42, fg, dim);
                drawFlatButton(mem, smallFont, m_rcPartInsert, m_args.zh ? L"插入" : L"Insert",
                               ed, false, 43, fg, dim);
                drawFlatButton(mem, smallFont, m_rcPartDelete, m_args.zh ? L"删除" : L"Delete",
                               ed && !empty, false, 44, fg, dim);
            }
        }
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
    bool canUndo() const { return m_kind == "env" ? m_env.canUndo() : m_hosts.canUndo(); }
    bool canRedo() const { return m_kind == "env" ? m_env.canRedo() : m_hosts.canRedo(); }

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
        MessageBoxW(m_hwnd, wideFromUtf8(body).c_str(),
                    wideFromUtf8(zh ? "权限说明" : "Permissions").c_str(), MB_OK);
    }

    const wchar_t* chipLabel(int id) const
    {
        const bool zh = m_args.zh;
        switch (id) {
        case kChipAdd: return zh ? L"Ins 新增" : L"Ins add";
        case kChipDel: return currentRowDeleted()
                           ? (zh ? L"Del 恢复" : L"Del restore")
                           : (zh ? L"Del 删除" : L"Del delete");
        case kChipEdit: return zh ? L"Enter 编辑" : L"Enter edit";
        case kChipUndo: return zh ? L"Ctrl+Z 撤销" : L"Ctrl+Z undo";
        case kChipRedo: return zh ? L"Ctrl+Y 重做" : L"Ctrl+Y redo";
        case kChipSave: return zh ? L"Ctrl+S 保存" : L"Ctrl+S save";
        case kChipClose: return zh ? L"Esc 关闭" : L"Esc close";
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
        kFlatPartDelete = 44
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
        if (delimiterApplyEnabled() && rectHasPoint(m_rcApply, p))
            return kFlatApply;
        if (!m_delimCustom && m_listMode && rowEditable(m_sel) && rectHasPoint(m_rcDelimiter, p))
            return kFlatDelim;
        if (!m_listMode)
            return 0;
        if (rectHasPoint(m_rcPartUp, p) && rowEditable(m_sel) && m_partSel > 0)
            return kFlatPartUp;
        if (rectHasPoint(m_rcPartDown, p) && rowEditable(m_sel)
            && m_partSel + 1 < static_cast<int>(m_valueParts.size()))
            return kFlatPartDown;
        if (rectHasPoint(m_rcPartInsert, p) && rowEditable(m_sel))
            return kFlatPartInsert;
        if (rectHasPoint(m_rcPartDelete, p) && rowEditable(m_sel) && !m_valueParts.empty())
            return kFlatPartDelete;
        return 0;
    }
    void drawFlatButton(HDC dc, HFONT font, const RECT& r, const wchar_t* label, bool enabled,
                        bool active, int id, COLORREF fg, COLORREF dim)
    {
        if (r.left >= r.right || r.top >= r.bottom)
            return;
        const bool hovered = enabled && m_hover == id;
        COLORREF bg;
        if (!enabled)
            bg = m_dark ? RGB(42, 45, 50) : RGB(242, 242, 242);
        else if (active)
            bg = accent();
        else if (hovered)
            bg = m_dark ? RGB(60, 66, 76) : RGB(215, 228, 244);
        else
            bg = m_dark ? RGB(48, 52, 58) : RGB(225, 232, 241);
        HBRUSH b = CreateSolidBrush(bg);
        FillRect(dc, &r, b);
        DeleteObject(b);
        HPEN p = CreatePen(PS_SOLID, 1, enabled
                                      ? (active ? accentEdge() : (m_dark ? RGB(90, 98, 110) : RGB(176, 188, 202)))
                                      : (m_dark ? RGB(60, 64, 70) : RGB(220, 222, 226)));
        HGDIOBJ old = SelectObject(dc, p);
        // Rectangle() fills with the selected brush (white by default), which
        // used to wipe the accent fill and leave white text on white.
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, old);
        DeleteObject(p);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? (active ? RGB(255, 255, 255) : fg) : dim);
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
    static COLORREF accent() { return RGB(88, 166, 255); }
    static COLORREF accentEdge() { return RGB(62, 132, 214); }

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
        COLORREF bg1 = m_dark ? RGB(36, 40, 46) : RGB(232, 236, 242);
        COLORREF t1 = m_extChanged ? RGB(180, 120, 20) : fg;
        if (flash) {
            bg1 = kind == 2 ? RGB(252, 235, 235)
                : kind == 1 ? RGB(250, 238, 218)
                : kind == 3 ? RGB(234, 243, 222)
                            : (m_dark ? RGB(36, 48, 62) : RGB(230, 241, 251));
            t1 = kind == 2 ? RGB(163, 45, 45)
               : kind == 1 ? RGB(133, 79, 11)
               : kind == 3 ? RGB(59, 109, 22)
                           : (m_dark ? RGB(180, 215, 255) : RGB(12, 68, 124));
        }
        HBRUSH b1 = CreateSolidBrush(bg1);
        FillRect(dc, &m_rcStatusRow1, b1);
        DeleteObject(b1);
        HBRUSH b2 = CreateSolidBrush(m_dark ? RGB(30, 33, 38) : RGB(241, 243, 247));
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
        HBRUSH ib = CreateSolidBrush(m_dark ? RGB(48, 52, 58) : RGB(255, 255, 255));
        FillRect(dc, &ri, ib);
        DeleteObject(ib);
        HBRUSH ie = CreateSolidBrush(elevatedNow ? (m_dark ? RGB(90, 96, 104) : RGB(190, 196, 204))
                                                  : (m_dark ? RGB(170, 130, 40) : RGB(202, 138, 30)));
        FrameRect(dc, &ri, ie);
        DeleteObject(ie);
        RECT it = ri;
        SetTextColor(dc, elevatedNow ? (m_dark ? RGB(180, 215, 255) : RGB(12, 68, 124))
                                     : (m_dark ? RGB(235, 190, 100) : RGB(150, 98, 10)));
        DrawTextW(dc, m_args.zh ? L"权限" : L"Perms",
                  -1, &it, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        for (Chip c : m_chips) {
            const bool en = chipEnabled(c.id);
            HBRUSH cb = CreateSolidBrush(en ? (m_dark ? RGB(48, 52, 58) : RGB(255, 255, 255))
                                            : (m_dark ? RGB(40, 43, 48) : RGB(236, 238, 241)));
            FillRect(dc, &c.rc, cb);
            DeleteObject(cb);
            HBRUSH edge = CreateSolidBrush(en ? (m_dark ? RGB(90, 96, 104) : RGB(190, 196, 204))
                                              : (m_dark ? RGB(55, 58, 64) : RGB(220, 222, 226)));
            FrameRect(dc, &c.rc, edge);
            DeleteObject(edge);
            SetTextColor(dc, en ? fg : dim);
            DrawTextW(dc, chipLabel(c.id), -1, &c.rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, of);
    }

    void drawHeader(HDC dc, COLORREF fg, COLORREF dim)
    {
        const RECT band {m_rcTable.left, m_rcTable.top, m_rcTable.right, m_rcTable.top + m_headerH};
        HBRUSH hb = CreateSolidBrush(m_dark ? RGB(42, 46, 53) : RGB(238, 241, 246));
        FillRect(dc, &band, hb);
        DeleteObject(hb);
        SetTextColor(dc, dim);
        const wchar_t* labels[5] = {
            m_kind == "env" ? (m_args.zh ? L"名称" : L"Name") : (m_args.zh ? L"启用" : L"On"),
            m_kind == "env" ? (m_args.zh ? L"类型" : L"Type") : L"IP",
            m_kind == "env" ? (m_args.zh ? L"值" : L"Value") : (m_args.zh ? L"主机名" : L"Hostname"),
            m_args.zh ? L"注释" : L"Comment",
            m_args.zh ? L"行" : L"Line"};
        for (int c = 0; c < m_colCount; ++c) {
            RECT r {m_colX[c] + 6, band.top, m_colX[c + 1] - 4, band.bottom};
            DrawTextW(dc, labels[c], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        HPEN p = CreatePen(PS_SOLID, 1, m_dark ? RGB(70, 76, 84) : RGB(200, 206, 214));
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
        const COLORREF removed = m_dark ? RGB(220, 120, 120) : RGB(170, 60, 60);
        if (m_kind == "env") {
            const auto& r = m_env.rows[m_visible[v]];
            const COLORREF c = r.deleted ? removed : normal;
            drawCell(dc, r.name, 0, row, c);
            drawCell(dc, r.type, 1, row, c);
            drawCell(dc, r.value, 2, row, c);
        } else {
            const auto& r = m_hosts.rows[m_visible[v]];
            const COLORREF c = r.deleted ? removed : (r.commented ? (m_dark ? RGB(130, 136, 144) : RGB(130, 130, 130)) : normal);
            if (r.isSection) {
                // Group headings span the whole row: drawing them in column 0
                // (8% of the width) truncated every comment to "# Copy...".
                HBRUSH hb = CreateSolidBrush(m_dark ? RGB(36, 40, 46) : RGB(238, 241, 246));
                FillRect(dc, &row, hb);
                DeleteObject(hb);
                RECT sr {row.left + 8, row.top, row.right - 8, row.bottom};
                SetTextColor(dc, m_dark ? RGB(130, 136, 144) : RGB(110, 110, 110));
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

    void drawBackupList(HDC dc, COLORREF fg, COLORREF dim)
    {
        SetTextColor(dc, dim);
        int y = m_rcTable.top + 4;
        if (m_backupIds.empty()) {
            std::wstring e = wideFromUtf8(m_args.zh ? "还没有备份" : "No backups yet");
            RECT rr {m_rcTable.left, y, m_rcTable.right, y + m_rowH};
            DrawTextW(dc, e.c_str(), -1, &rr, DT_LEFT | DT_SINGLELINE);
            return;
        }
        const int first = std::min(m_topRow, maxTopRow());
        const int last = std::min<int>(rowCount(), first + pageRows());
        for (int i = first; i < last; ++i) {
            const int yy = m_rcTable.top + 4 + (i - first) * m_rowH;
            RECT rr {m_rcTable.left, yy, m_rcTable.right, yy + m_rowH};
            if (i == m_sel) {
                HBRUSH hb = CreateSolidBrush(m_dark ? RGB(38, 42, 48) : RGB(210, 228, 250));
                FillRect(dc, &rr, hb);
                DeleteObject(hb);
            }
            SetTextColor(dc, fg);
            rr.left += 8;
            DrawTextW(dc, wideFromUtf8(m_backupIds[i]).c_str(), -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    std::string m_errorBanner() const { return {}; }
    void save()
    {
        endEdit();
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
            setStatus(m_args.zh ? "有无效项，无法保存（检查名称/值）" : "Invalid entries; cannot save", 2);
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
        std::string body = (m_args.zh ? "保存 " : "Save ") + std::to_string(details.size())
                           + (m_args.zh ? " 项修改：" : " changes:") + "\n\n";
        for (std::size_t i = 0; i < details.size() && i < 8; ++i)
            body += details[i] + "\n";
        if (details.size() > 8)
            body += "... +" + std::to_string(details.size() - 8) + "\n";
        body += "\n" + std::string(m_args.zh ? "确认写入？" : "Write now?");
        if (MessageBoxW(m_hwnd, wideFromUtf8(body).c_str(),
                        wideFromUtf8(m_args.zh ? "确认保存" : "Confirm save").c_str(), MB_YESNO) != IDYES)
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
            if (fpNow != (sysW ? m_env.fingerprint : m_env.userFingerprint)) {
                // Ask with the shared lock released: the dialog can stay open for
                // minutes, and holding g_writeLock would block the protocol thread.
                lock.unlock();
                const std::string body = m_args.zh
                    ? std::string(sysW ? "环境变量" : "用户环境变量") + "在编辑器打开后被外部修改。\n\n"
                      + "中止：取消本次保存\n重试：重新载入最新值（丢弃未保存的修改）\n忽略：用当前编辑强制覆盖"
                    : std::string(sysW ? "The environment" : "The user environment") + " changed while this window was open.\n\n"
                      + "Abort: cancel this save\nRetry: reload the latest values (discards unsaved edits)\n"
                      + "Ignore: overwrite with the current edits";
                const int answer = MessageBoxW(m_hwnd, wideFromUtf8(body).c_str(),
                                               wideFromUtf8(m_args.zh ? "外部已修改" : "External change").c_str(),
                                               MB_ABORTRETRYIGNORE | MB_ICONWARNING | MB_DEFBUTTON1);
                lock.lock();
                if (answer == IDABORT) {
                    setStatus(m_args.zh ? "已取消保存" : "Save cancelled", 1);
                    return;
                }
                if (answer == IDRETRY) {
                    reloadFromDisk();
                    setStatus(m_args.zh ? "已重新载入，请重新编辑后再保存" : "Reloaded; re-apply your edits", 0);
                    return;
                }
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
            clearDraft();
            if (failed.empty())
                setStatus((m_args.zh ? "已保存 " : "Saved ") + std::to_string(ok) + (m_args.zh ? " 项" : ""), 3);
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
            if (envh::edit::HostsModel::contentFingerprint(rr.content) != m_hosts.fingerprint) {
                setStatus(m_args.zh ? "外部已修改，已取消保存（重新载入后再试）" : "External change; save cancelled", 1);
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
            clearDraft();
            setStatus(m_args.zh ? "已保存 hosts" : "Saved hosts", 3);
        }
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
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        if (m_kind == "env")
            for (const auto& b : backups.listEnv())
                m_backupIds.push_back(b.id);
        else
            for (const auto& b : backups.listHosts())
                m_backupIds.push_back(b.id);
    }

    void loadBackupIntoWork(const std::string& id)
    {
        envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
        if (m_kind == "env") {
            const std::string path = m_args.dataDir + "/backups/env/" + id + ".json";
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return;
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto j = winshun::Json::parse(text);
            if (!j)
                return;
            // The backup is the target state for the scopes it holds; scopes it
            // does not hold (system, in a user-only backup) come from a live
            // read, so loading it never marks them deleted.
            envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
            const envh::EnvSnapshot live = store.read(m_args.showSystem).snap;
            envh::EnvSnapshot snap;
            bool hasSystem = false;
            for (const winshun::Json& v : (*j)["vars"].toArray()) {
                envh::EnvVar e;
                e.scope = v["scope"].toString();
                e.name = v["name"].toString();
                e.type = v["type"].toString();
                e.value = v["value"].toString();
                hasSystem = hasSystem || e.scope == "system";
                snap.vars.push_back(std::move(e));
            }
            for (const envh::EnvVar& v : live.vars)
                if (v.scope == "system" && !hasSystem)
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
    bool canDeleteRow() const
    {
        if (m_tab == 2 || m_visible.empty())
            return false;
        const int i = m_visible[m_sel];
        if (m_kind == "env")
            return m_env.rows[i].scope == "user" || (m_env.rows[i].scope == "system" && m_args.systemWritable);
        return m_tab == 0 && m_args.writable && !m_hosts.rows[i].isSection;
    }
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
        const int v = findVisible(idx);
        selectRow(v < 0 ? 0 : v);
        SetFocus(m_kind == "env" ? m_nameEdit : m_ipEdit);
        setStatus(m_args.zh ? "已新增一行（未保存）" : "Row added (unsaved)", 3);
    }
    void doDelete()
    {
        endEdit();
        if (!canDeleteRow())
            return;
        const int idx = m_visible[m_sel];
        const bool restoring = currentRowDeleted();
        const bool isNew = m_kind == "env" ? !m_env.rows[idx].inBase
                                           : m_hosts.rows[idx].origin < 0;
        if (m_kind == "env") {
            if (m_env.rows[idx].inBase)
                m_env.toggleDeleted(idx);
            else
                m_env.removeRow(idx);
        } else {
            if (m_hosts.rows[idx].origin >= 0)
                m_hosts.toggleDeleted(idx);
            else
                m_hosts.removeRow(idx);
        }
        rebuildVisible();
        selectRow(m_sel);
        setStatus(isNew ? (m_args.zh ? "已移除新增行" : "New row removed")
                  : restoring ? (m_args.zh ? "已恢复该行" : "Row restored")
                              : (m_args.zh ? "已标记删除（再按 Del 恢复）" : "Marked deleted (Del again restores)"), 3);
    }
    void doUndo()
    {
        endEdit();
        if (m_kind == "hosts" && m_tab == 1) {
            SendMessageW(m_rawEdit, EM_UNDO, 0, 0); // text-level undo inside the raw box
            return;
        }
        if (m_kind == "env")
            m_env.undo();
        else
            m_hosts.undo();
        rebuildVisible();
        selectRow(m_sel);
    }
    void doRedo()
    {
        endEdit();
        if (m_kind == "env")
            m_env.redo();
        else
            m_hosts.redo();
        rebuildVisible();
        selectRow(m_sel);
    }
    void doEdit()
    {
        endEdit();
        if (!rowEditable(m_sel))
            return;
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
        }
    }
    void switchTab(int i)
    {
        if (m_tab == i)
            return;
        // Leaving the raw tab applies its text to the model (like any other
        // edit in this window, nothing reaches the file before Ctrl+S).
        if (m_kind == "hosts" && m_tab == 1)
            commitRaw();
        m_topRowByTab[m_tab] = m_topRow;
        m_tab = i;
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
        // Native child controls (EDIT / LISTBOX / COMBOBOX / BUTTON) erase and
        // paint with the system colors, which stay light in the dark theme and
        // make the detail fields look like holes cut in the panel.
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            if (!m_dark) {
                // The checkbox sits on the light-grey detail panel; the default
                // brush painted a white/grey block behind "启用".
                if (msg == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lp) == m_enabledChk) {
                    if (!m_lightPanelBrush)
                        m_lightPanelBrush = CreateSolidBrush(RGB(246, 248, 251));
                    HDC dc = reinterpret_cast<HDC>(wp);
                    SetBkColor(dc, RGB(246, 248, 251));
                    SetTextColor(dc, RGB(32, 39, 51));
                    return reinterpret_cast<LRESULT>(m_lightPanelBrush);
                }
                return DefWindowProcW(m_hwnd, msg, wp, lp);
            }
            HDC dc = reinterpret_cast<HDC>(wp);
            const HWND h = reinterpret_cast<HWND>(lp);
            const bool fieldLike = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || h == m_rawEdit;
            const COLORREF bk = fieldLike ? RGB(22, 24, 28) : RGB(30, 33, 38);
            if (!m_darkFieldBrush)
                m_darkFieldBrush = CreateSolidBrush(RGB(22, 24, 28));
            if (!m_darkBgBrush)
                m_darkBgBrush = CreateSolidBrush(RGB(30, 33, 38));
            SetTextColor(dc, fieldLike ? RGB(232, 234, 237) : RGB(200, 206, 214));
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
            if (HIWORD(wp) == LBN_SELCHANGE && reinterpret_cast<HWND>(lp) == m_partsList) {
                if (m_partsGuard > 0)
                    return 0;
                // The list already moved its own caret and scroll position. Read
                // the new index first: commitPartEdit() re-asserts the previous
                // selection, which would otherwise snap the list back and make a
                // click look like a jump to the top.
                const int sel = static_cast<int>(SendMessageW(m_partsList, LB_GETCURSEL, 0, 0));
                commitPartEdit();
                if (sel >= 0 && sel < static_cast<int>(m_valueParts.size()))
                    m_partSel = sel;
                SendMessageW(m_partsList, LB_SETCURSEL, m_partSel, 0);
                showCurrentPart();
                invalidatePartChrome();
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
            const bool rawTab = (m_kind == "hosts" && m_tab == 1);
            if (!rawTab) {
                const int baseY = m_rcTable.top + (m_tab == 2 ? 4 : m_headerH);
                if (y >= baseY && y < m_rcTable.bottom) {
                    const int v = m_topRow + (y - baseY) / m_rowH;
                    if (v >= 0 && v < rowCount()) {
                        selectRow(v);
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
            if (inRect(m_rcInfo, pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (m_kind == "env" && m_tab != 2 && inRect(m_rcSplitter, pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
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
                const int r = MessageBoxW(m_hwnd,
                                          wideFromUtf8(m_args.zh ? "有未保存的修改，保存后再关闭？" : "Unsaved changes; save before closing?").c_str(),
                                          wideFromUtf8(m_args.zh ? "关闭" : "Close").c_str(),
                                          MB_YESNOCANCEL);
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

    LRESULT onKey(WPARAM wp)
    {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            if (wp == 'S' || wp == 's') { save(); return 0; }
            if (wp == 'Z' || wp == 'z') { doUndo(); return 0; }
            if (wp == 'Y' || wp == 'y') { doRedo(); return 0; }
            if (wp == 'F' || wp == 'f') { if (m_filterEdit) SetFocus(m_filterEdit); return 0; }
            if (wp == 'R' || wp == 'r') {
                toggleListMode();
                return 0;
            }
            if (wp == VK_TAB) {
                // Ctrl+Tab can arrive while a detail field has focus, and the
                // tab being left may hide that field, so the table keeps focus.
                switchTab((m_tab + ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 2 : 1)) % 3);
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
                const int idx = m_sel % static_cast<int>(m_backupIds.size());
                loadBackupIntoWork(m_backupIds[idx]);
            }
            return 0;
        }
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (m_kind == "env" && m_listMode && !ctrl) {
            if (wp == VK_INSERT) { addPart(); return 0; }
            if (wp == VK_DELETE) { deletePart(); return 0; }
            if (alt && wp == VK_UP) { movePart(-1); return 0; }
            if (alt && wp == VK_DOWN) { movePart(1); return 0; }
            if (wp == VK_RETURN) { commitPartEdit(); SetFocus(m_hwnd); return 0; }
        }
        if (wp == VK_ESCAPE) {
            if (m_kind == "env" ? m_envDirty() : m_hostsDirty()) {
                const int r = MessageBoxW(m_hwnd,
                                          wideFromUtf8(m_args.zh ? "有未保存的修改，保存后再关闭？" : "Unsaved changes; save before closing?").c_str(),
                                          wideFromUtf8(m_args.zh ? "关闭" : "Close").c_str(), MB_YESNOCANCEL);
                if (r == IDYES) { save(); persistDraftIfDirty(); }
                else if (r == IDNO) { persistDraftIfDirty(); }
                else return 0;
            }
            DestroyWindow(m_hwnd);
            return 0;
        }
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
                    d = wp == VK_PRIOR ? -p : p;
                }
                selectRow(m_sel + d);
            }
            return 0;
        }
        if (wp == VK_TAB) {
            switchTab((m_tab + 1) % 3);
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_F2) {
            if (m_kind == "env") doEdit();
            else if (!m_visible.empty() && rowEditable(m_sel)) {
                auto& r = m_hosts.rows[m_visible[m_sel]];
                m_hosts.setEnabled(m_visible[m_sel], r.commented);
                rebuildVisible();
                selectRow(m_sel);
            }
            return 0;
        }
        if (wp == VK_SPACE && m_kind == "hosts" && !m_visible.empty()) {
            if (rowEditable(m_sel)) {
                auto& r = m_hosts.rows[m_visible[m_sel]];
                m_hosts.setEnabled(m_visible[m_sel], r.commented);
                rebuildVisible();
                selectRow(m_sel);
            }
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
