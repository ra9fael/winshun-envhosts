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

namespace editorwin {

// ---- arguments the protocol thread hands to the window -----------------------
struct OpenArgs {
    std::string kind; // "env" | "hosts"
    std::wstring userSubkey = L"Environment"; // HKCU subkey (test override allowed)
    std::wstring hostsPath; // full path to hosts
    std::string dataDir; // WS_DATA_DIR (backups/ and drafts/<kind>.json live here)
    int backupCount = 10;
    bool showSystem = true;
    std::string filter;
    std::string select; // "user/NAME" (env) or 1-based line (hosts)
    winshun::Json draft; // empty => ignore
    bool zh = true;
    bool dark = false;
    bool writable = true; // hosts writability probe at open time
};

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
        const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_VSCROLL;
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
    }

    void externalChange()
    {
        if (m_envDirty() || m_hostsDirty()) {
            m_extChanged = true; // yellow bar; save will surface the conflict
            InvalidateRect(m_hwnd, nullptr, TRUE);
        } else {
            loadModel(); // silent reload
            selectRow(0);
        }
    }

    void persistDraftIfDirty()
    {
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
    bool m_listMode = false; // env PATH list/raw toggle
    bool m_extChanged = false;
    bool m_dark = false;
    bool m_loading = false; // suppress EN_CHANGE while refreshDetail fills controls

    // controls
    HWND m_filterEdit = nullptr;
    HWND m_nameEdit = nullptr, m_typeEdit = nullptr, m_valueEdit = nullptr;
    HWND m_enabledChk = nullptr, m_ipEdit = nullptr, m_hostEdit = nullptr, m_commentEdit = nullptr;
    HWND m_rawEdit = nullptr;
    HFONT m_editFont = nullptr;

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
    RECT m_rcStatus = {};
    RECT m_rcStatusRow1 = {}, m_rcStatusRow2 = {};
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
            envh::native::EnvStore store(m_args.userSubkey);
            const envh::native::EnvReadResult r = store.read(m_args.showSystem);
            m_env = envh::edit::EnvModel::fromSnapshot(r.snap);
            m_hasEnv = true;
            // Auto-restore a leftover draft from a previous dirty close.
            const std::string draft = readDraft();
            if (!draft.empty()) {
                const auto j = winshun::Json::parse(draft);
                if (j && !j->isNull())
                    loadDraft(*j, /*markDirty=*/true);
            }
        } else {
            envh::native::HostsStore store(m_args.hostsPath);
            const envh::native::HostsReadResult r = store.read();
            if (r.ok) {
                const envh::HostsParsed p = envh::parseHosts(r.content);
                bool crlf = false;
                for (const auto& l : p.lines)
                    if (!l.text.empty())
                        crlf = l.crlf;
                m_hosts = envh::edit::HostsModel::fromParsed(p, crlf);
                m_hasHosts = true;
            }
            const std::string draft = readDraft();
            if (!draft.empty()) {
                const auto j = winshun::Json::parse(draft);
                if (j && !j->isNull())
                    loadDraft(*j, /*markDirty=*/true);
            }
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

    void selectByRef(const std::string& ref)
    {
        if (m_kind == "env") {
            for (int i = 0; i < static_cast<int>(m_env.rows.size()); ++i) {
                if (m_env.rows[i].scope + "/" + m_env.rows[i].name == ref) {
                    rebuildVisible();
                    for (std::size_t v = 0; v < m_visible.size(); ++v)
                        if (m_visible[v] == i) {
                            m_sel = static_cast<int>(v);
                            refreshDetail();
                            return;
                        }
                }
            }
        } else {
            // ref is a 1-based line number
            const int line = std::atoi(ref.c_str());
            for (int i = 0; i < static_cast<int>(m_hosts.rows.size()); ++i) {
                if (m_hosts.rows[i].origin + 1 == line) {
                    rebuildVisible();
                    for (std::size_t v = 0; v < m_visible.size(); ++v)
                        if (m_visible[v] == i) {
                            m_sel = static_cast<int>(v);
                            refreshDetail();
                            return;
                        }
                }
            }
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
        if (self && msg == WM_KEYDOWN) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
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
                return CallWindowProcW(old, h, msg, wp, lp);
            }
            if (wp == VK_ESCAPE && !ctrl) {
                self->endEdit();
                SetFocus(self->m_hwnd);
                return 0;
            }
            if (wp == VK_TAB || wp == VK_INSERT || wp == VK_DELETE || wp == VK_PRIOR
                || wp == VK_NEXT || wp == VK_F5
                || (ctrl && (wp == 'Z' || wp == 'z' || wp == 'Y' || wp == 'y'
                              || wp == 'S' || wp == 's'))) {
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
        // hosts detail
        m_enabledChk = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 16, 16,
                                       m_hwnd, reinterpret_cast<HMENU>(5), GetModuleHandleW(nullptr), nullptr);
        subclassEdit(m_enabledChk);
        m_ipEdit = makeEdit(es, 6);
        m_hostEdit = makeEdit(es, 7);
        m_commentEdit = makeEdit(es, 8);
        // hosts raw tab
        m_rawEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 10, 10,
                                    m_hwnd, reinterpret_cast<HMENU>(9), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(m_rawEdit, EM_FMTLINES, 0, 0);
        if (m_rawEdit && m_editFont)
            SendMessageW(m_rawEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_editFont), TRUE);
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
        const int detailH = 112;
        const int statusH = 48; // two rows: context / actions
        m_rcTable = {8, 34, m_clientW - 8, m_clientH - detailH - statusH - 8};
        m_rcDetail = {8, m_clientH - detailH - statusH, m_clientW - 8, m_clientH - statusH};
        m_rcStatus = {0, m_clientH - statusH, m_clientW, m_clientH};
        m_rcStatusRow1 = {0, m_clientH - statusH, m_clientW, m_clientH - statusH + 24};
        m_rcStatusRow2 = {0, m_clientH - statusH + 24, m_clientW, m_clientH};

        // Action buttons occupy the right edge of the detail panel.
        const int bw = 46, gap = 4;
        int bx = m_rcDetail.right - 4 * bw - 3 * gap - 4;
        const int by = m_rcDetail.top + 20;
        m_rcAddBtn = {bx, by, bx + bw, by + 22}; bx += bw + gap;
        m_rcDelBtn = {bx, by, bx + bw, by + 22}; bx += bw + gap;
        m_rcUndoBtn = {bx, by, bx + bw, by + 22}; bx += bw + gap;
        m_rcRedoBtn = {bx, by, bx + bw, by + 22};
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
            if (raw)
                MoveWindow(m_rawEdit, m_rcTable.left, m_rcTable.top,
                           m_rcTable.right - m_rcTable.left, m_rcTable.bottom - m_rcTable.top, TRUE);
        }
        if (!showDetail || raw) {
            for (HWND e : {m_nameEdit, m_typeEdit, m_valueEdit, m_enabledChk, m_ipEdit, m_hostEdit, m_commentEdit})
                if (e)
                    ShowWindow(e, SW_HIDE);
            return;
        }
        // Position the edit controls directly under the labels drawn in paint():
        // label row 1 at detail.top+2, controls at +20, label row 2 at +44,
        // value control at +60.
        const int lx = m_rcDetail.left + 4;
        const int w = m_rcDetail.right - m_rcDetail.left - 8 - 200; // reserve the action buttons
        const int top = m_rcDetail.top + 20;
        const int valTop = m_rcDetail.top + 60;
        if (m_kind == "env") {
            show(m_nameEdit, lx, top, w / 2 - 6, 22);
            show(m_typeEdit, lx + w / 2 + 4, top, w / 2 - 6, 22);
            const int valH = std::max(24, static_cast<int>(m_rcDetail.bottom - valTop - 6));
            show(m_valueEdit, lx, valTop, w, valH);
        } else {
            show(m_enabledChk, lx, top + 3, 16, 16);
            show(m_ipEdit, lx + 24, top, w / 3 - 30, 22);
            show(m_hostEdit, lx + w / 3, top, w / 3, 22);
            show(m_commentEdit, lx + 2 * w / 3 + 4, top, w / 3 - 4, 22);
        }
        refreshDetail();
    }
    void show(HWND e, int x, int y, int w, int h)
    {
        if (!e)
            return;
        ShowWindow(e, SW_SHOW);
        MoveWindow(e, x, y, w, h, TRUE);
    }

    // Fill the detail controls from the selected row (no model write).
    void refreshDetail()
    {
        // SetWindowTextW fires EN_CHANGE; without this guard commitDetail() would
        // write half-filled controls back into the model and mark it dirty.
        m_loading = true;
        if (m_kind == "env") {
            std::string name, type, value;
            bool ro = false;
            if (!m_visible.empty()) {
                const auto& r = m_env.rows[m_visible[m_sel]];
                name = r.name;
                type = r.type;
                value = r.value;
                ro = (r.scope == "system");
            }
            setEdit(m_nameEdit, name, ro);
            setEdit(m_typeEdit, type, ro);
            setEdit(m_valueEdit, value, ro);
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
            if (m_enabledChk)
                SendMessageW(m_enabledChk, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
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
            const std::string name = getEdit(m_nameEdit), value = getEdit(m_valueEdit);
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
            nSecond = static_cast<int>(m_hosts.rows.size());
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

        // read-only / error banner
        if ((m_kind == "hosts" && !m_args.writable) || !m_errorBanner().empty()) {
            RECT b {0, 30, w, 50};
            HBRUSH bb = CreateSolidBrush(RGB(120, 60, 60));
            FillRect(mem, &b, bb);
            DeleteObject(bb);
            SetTextColor(mem, errFg);
            std::wstring msg = wideFromUtf8(m_kind == "hosts" && !m_args.writable
                                               ? (m_args.zh ? "只读：当前没有写权限，无法保存"
                                                            : "Read-only: no write permission")
                                               : m_errorBanner());
            RECT bt = b; bt.left += 10;
            DrawTextW(mem, msg.c_str(), -1, &bt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // ---- table: header band, column separators, rows ----
        if (m_tab != 2) {
            drawHeader(mem, fg, dim);
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
            const int dw = m_rcDetail.right - m_rcDetail.left - 8 - 200;
            if (m_kind == "env") {
                RECT a {lx, m_rcDetail.top + 2, lx + dw / 2 - 6, m_rcDetail.top + 18};
                RECT b {lx + dw / 2 + 4, a.top, m_rcDetail.right - 4, a.bottom};
                RECT c {lx, m_rcDetail.top + 44, m_rcDetail.right - 4, m_rcDetail.top + 60};
                DrawTextW(mem, m_args.zh ? L"名称" : L"Name", -1, &a, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, m_args.zh ? L"类型" : L"Type", -1, &b, DT_LEFT | DT_SINGLELINE);
                const wchar_t* vlab = m_args.zh
                    ? (m_listMode ? L"值（列表，每行一段；Ctrl+R 回原文）" : L"值（Ctrl+R 切换列表模式）")
                    : (m_listMode ? L"Value (list; Ctrl+R raw)" : L"Value (Ctrl+R list)");
                DrawTextW(mem, vlab, -1, &c, DT_LEFT | DT_SINGLELINE);
            } else {
                RECT b {lx + 24, m_rcDetail.top + 2, lx + dw / 3, m_rcDetail.top + 18};
                RECT c {lx + dw / 3, b.top, lx + 2 * dw / 3 + 4, b.bottom};
                RECT d {lx + 2 * dw / 3 + 4, b.top, m_rcDetail.right - 4, b.bottom};
                // The checkbox is self-explanatory; a label here would overflow
                // its narrow column and collide with "IP" (two CJK glyphs > 20px).
                DrawTextW(mem, L"IP", -1, &b, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, m_args.zh ? L"主机名" : L"Hostname", -1, &c, DT_LEFT | DT_SINGLELINE);
                DrawTextW(mem, m_args.zh ? L"注释" : L"Comment", -1, &d, DT_LEFT | DT_SINGLELINE);
            }
            SelectObject(mem, of);
        }

        // ---- detail action buttons + two-row status bar ----
        drawActionButtons(mem, smallFont, fg, dim);
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

    bool isDirty() const { return m_kind == "env" ? m_envDirty() : m_hostsDirty(); }
    bool canUndo() const { return m_kind == "env" ? m_env.canUndo() : m_hosts.canUndo(); }
    bool canRedo() const { return m_kind == "env" ? m_env.canRedo() : m_hosts.canRedo(); }

    bool rowEditable(int v) const
    {
        if (m_tab == 2 || v < 0 || v >= static_cast<int>(m_visible.size()))
            return false;
        const int i = m_visible[v];
        if (m_kind == "env")
            return m_env.rows[i].scope == "user" && !m_env.rows[i].deleted;
        if (m_tab == 1 || !m_args.writable)
            return false;
        return !m_hosts.rows[i].isSection && !m_hosts.rows[i].deleted;
    }
    bool canAddRow() const
    {
        if (m_tab == 2)
            return false;
        if (m_kind == "env")
            return m_tab == 0;
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
            s += " · " + std::to_string(n) + (zh ? " 项变更" : " changes");
        }
        const int nRows = rowCount();
        if (nRows > 0) {
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
                 ? (zh ? " · 用户可写 / 系统只读" : " · user writable / system read-only")
                 : (m_args.writable ? (zh ? " · 可写" : " · writable") : (zh ? " · 只读" : " · read-only"));
        return s;
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
        HGDIOBJ of = SelectObject(dc, font);
        for (int i = 0; i < 4; ++i) {
            HBRUSH b = CreateSolidBrush(
                en[i] ? (i < 2 ? RGB(225, 245, 238) : (m_dark ? RGB(48, 52, 58) : RGB(238, 241, 246)))
                      : (m_dark ? RGB(42, 45, 50) : RGB(242, 242, 242)));
            FillRect(dc, &rects[i], b);
            DeleteObject(b);
            HBRUSH edge = CreateSolidBrush(
                en[i] ? (i < 2 ? RGB(29, 158, 117) : RGB(180, 186, 196))
                      : (m_dark ? RGB(60, 64, 70) : RGB(220, 222, 226)));
            FrameRect(dc, &rects[i], edge);
            DeleteObject(edge);
            SetTextColor(dc, en[i] ? fg : dim);
            DrawTextW(dc, labels[i], -1, &rects[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, of);
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
        r1.right -= 8;
        DrawTextW(dc, wideFromUtf8(statusRow1Text()).c_str(), -1, &r1,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
                DrawTextW(dc, wideFromUtf8(r.sectionText).c_str(), -1, &sr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            } else {
                drawCell(dc, r.commented ? "[ ]" : "[x]", 0, row, c);
                drawCell(dc, r.ip, 1, row, c);
                drawCell(dc, join(r.hostnames), 2, row, c);
                drawCell(dc, r.comment, 3, row, c);
                drawCell(dc, std::to_string(r.origin + 1), 4, row, c);
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
        const bool dirty = isDirty();
        if (!dirty) {
            setStatus(m_args.zh ? "没有改动" : "No changes");
            return;
        }
        if (m_kind == "env" && m_env.hasInvalid()) {
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

        std::lock_guard<std::mutex> lock(envh::native::g_writeLock);
        if (m_kind == "env") {
            envh::native::EnvStore store(m_args.userSubkey);
            // conflict check
            const std::string fpNow = envh::edit::EnvModel::computeFingerprint(store.read(false).snap);
            if (fpNow != m_env.fingerprint) {
                setStatus(m_args.zh ? "外部已修改，已取消保存（重新载入后再试）" : "External change; save cancelled", 1);
                return;
            }
            envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
            backups.saveEnv(store.read(false).snap);
            const auto d = m_env.delta();
            int ok = 0;
            for (const auto& r : d.toWrite)
                if (store.writeVar(r.name, r.value, r.type))
                    ++ok;
            for (const auto& name : d.toDelete)
                if (store.deleteVar(name))
                    ++ok;
            store.broadcastSettingsChange();
            m_env = envh::edit::EnvModel::fromSnapshot(store.read(false).snap);
            clearDraft();
            setStatus((m_args.zh ? "已保存 " : "Saved ") + std::to_string(ok) + (m_args.zh ? " 项" : ""), 3);
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
            m_hosts = envh::edit::HostsModel::fromParsed(p, m_hosts.dominantCrlf());
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
            envh::EnvSnapshot snap;
            for (const winshun::Json& v : (*j)["vars"].toArray()) {
                envh::EnvVar e;
                e.scope = v["scope"].toString();
                e.name = v["name"].toString();
                e.type = v["type"].toString();
                e.value = v["value"].toString();
                snap.vars.push_back(std::move(e));
            }
            m_env = envh::edit::EnvModel::fromSnapshot(snap); // becomes dirty vs live base
        } else {
            const std::string path = m_args.dataDir + "/backups/hosts/" + id + ".hosts";
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return;
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const envh::HostsParsed p = envh::parseHosts(content);
            bool crlf = false;
            for (const auto& l : p.lines)
                if (!l.text.empty())
                    crlf = l.crlf;
            m_hosts = envh::edit::HostsModel::fromParsed(p, crlf);
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
            return m_env.rows[i].scope == "user";
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
            idx = static_cast<int>(m_env.addNew("user"));
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
        SetFocus(m_kind == "env" ? m_valueEdit : m_ipEdit);
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
        m_topRowByTab[m_tab] = m_topRow;
        m_tab = i;
        if (i == 2)
            refreshBackups();
        m_topRow = m_topRowByTab[i];
        rebuildVisible();
        clampScroll();
        selectRow(m_topRow);
        applyLayout();
        InvalidateRect(m_hwnd, nullptr, TRUE);
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
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE) {
                if (m_loading)
                    return 0; // programmatic fill, not a user edit
                if (reinterpret_cast<HWND>(lp) == m_filterEdit) {
                    // Typing in the filter box must actually filter the table.
                    m_filterText = getEdit(m_filterEdit);
                    rebuildVisible();
                    m_topRow = 0;
                    clampScroll();
                    selectRow(0);
                    InvalidateRect(m_hwnd, nullptr, TRUE);
                } else {
                    // live-commit detail edits (one undo step per edit session)
                    commitDetail();
                }
            }
            return 0;
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
        case WM_KEYDOWN:
            return onKey(wp);
        case WM_CLOSE:
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
            if (wp == 'R' || wp == 'r') { if (m_kind == "env") { m_listMode = !m_listMode; applyLayout(); } return 0; }
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
            else {
                if (!m_visible.empty()) {
                    auto& r = m_hosts.rows[m_visible[m_sel]];
                    m_hosts.setEnabled(m_visible[m_sel], !r.commented);
                    rebuildVisible();
                    selectRow(m_sel);
                }
            }
            return 0;
        }
        if (wp == VK_SPACE && m_kind == "hosts" && !m_visible.empty()) {
            auto& r = m_hosts.rows[m_visible[m_sel]];
            m_hosts.setEnabled(m_visible[m_sel], !r.commented);
            rebuildVisible();
            selectRow(m_sel);
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
            loadModel();
            rebuildVisible();
            selectRow(0);
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
