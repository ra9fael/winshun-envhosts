// main.cpp - WinShun resident plugin: Env & hosts editor.
//
// The SDK owns stdio and the JSON-RPC framing. This file maps queries to pure
// functions in logic.h, builds result rows, and answers actions by calling the
// Win32 layer in native.h. Queries NEVER write to the registry, the hosts file,
// the clipboard, or open a window: copy, setQuery, open and write effects are
// returned only from action() when the user triggers them. The "visual editing"
// is delivered through the launcher list itself - Enter rewrites the query box
// (setQuery) so the text is the state and survives restarts / late cancels.
//
// Code and comments are English; user-visible strings are localized with the
// host locale (Chinese by default, English when locale starts with "en").
//
// x64 is mandatory: a 32-bit build is redirected by the file-system redirector
// to SysWOW64 and would silently edit the wrong hosts file.
#include "winshun.h"
#include "build_info.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "logic.h"
#include "i18n.h"

#ifdef _WIN32
#include "native.h"
#ifdef ENVHOSTS_QML_EDITOR
#include <QGuiApplication>
#include "qt_window_icon.h"
#include <QMetaObject>
#include <thread>
#include "qt/qt_editor.h"
#else
#include "editor_win.h"
#endif
#endif

static_assert(sizeof(void*) == 8, "ra9fael.envhosts must be built as x64; a 32-bit build edits the wrong System32\\drivers\\etc\\hosts via the Wow64 redirector");

namespace {

struct Settings {
    std::string hostsPath = "%SystemRoot%\\System32\\drivers\\etc\\hosts";
    std::string sshDir;
    int backupCount = 10;
    int maxResults = 50;
    bool showSystem = true;
    std::string uiLanguage = "auto"; // auto | zh-CN | en-US
};

Settings readSettings(const winshun::Json& s)
{
    Settings st;
    const auto text = [&](const char* key, const std::string& fallback) -> std::string {
        const winshun::Json& v = s[key];
        if (v.isString())
            return v.toString();
        if (v.isNumber())
            return std::to_string(v.toInt());
        return fallback;
    };
    st.hostsPath = text("hostsPath", st.hostsPath);
    st.sshDir = text("sshDir", st.sshDir);
    st.uiLanguage = text("uiLanguage", st.uiLanguage);
    st.backupCount = envh::parseIntOr(text("backupCount", "10"), st.backupCount);
    st.maxResults = envh::parseIntOr(text("maxResults", "50"), st.maxResults);
    if (st.maxResults <= 0 || st.maxResults > 200)
        st.maxResults = 50;
    // Booleans arrive as JSON bools from the host; the string form is kept for
    // hand-edited settings files.
    const winshun::Json& sv = s["showSystemVars"];
    st.showSystem = sv.isBool() ? sv.toBool() : (text("showSystemVars", "true") != "false");
    return st;
}

std::string expandPathVars(const std::string& t)
{
    std::string out;
    std::size_t i = 0;
    while (i < t.size()) {
        if (t[i] == '%') {
            std::size_t j = i + 1;
            while (j < t.size() && t[j] != '%')
                ++j;
            if (j < t.size()) {
                const std::string name = t.substr(i + 1, j - i - 1);
                const std::string v = winshun::env(name.c_str());
                out += v.empty() ? t.substr(i, j - i + 1) : v;
                i = j + 1;
            } else {
                out += t[i];
                ++i;
            }
        } else {
            out += t[i];
            ++i;
        }
    }
    return out;
}

std::string tr(bool zh, const char* z, const char* e)
{
    return zh ? z : e;
}

winshun::Json rowData(std::initializer_list<std::pair<const char*, winshun::Json>> kv)
{
    winshun::Json::Object o;
    for (const auto& p : kv)
        o[p.first] = p.second;
    return winshun::Json(std::move(o));
}

winshun::Item errorRow(const std::string& id, const std::string& code, const std::string& title,
                       const std::string& subtitle, const std::string& detail, bool zh)
{
    winshun::Item item(id, title, subtitle);
    item.invalid()
        .withIcon("color:#C0392B")
        .withScore(0)
        .withCopy(subtitle)
        .withPreview(detail)
        .withData(rowData({{"kind", "error"}, {"code", code}, {"primary", subtitle}, {"detail", detail},
                           {"chain", ""}, {"expand", false}}));
    (void)zh;
    return item;
}

// ---- env rows -----------------------------------------------------------------

winshun::Item envVarRow(const envh::EnvVar& v, const std::string& expanded, bool writableScope, bool zh)
{
    const bool isUser = v.scope == "user";
    const std::string title = v.name + " = " + (v.value.size() > 90 ? v.value.substr(0, 90) + "…" : v.value);
    std::string subtitle;
    if (isUser)
        subtitle = tr(zh, "用户 · ", "user · ") + v.type;
    else
        subtitle = tr(zh, "系统 · ", "system · ") + v.type + (writableScope ? "" : tr(zh, " · 🔒 改写需管理员", " · 🔒 needs admin"));
    const bool expandable = v.type == "REG_EXPAND_SZ" && expanded != v.value;
    if (expandable)
        subtitle += tr(zh, " · 展开 ", " · expanded ") + std::to_string(expanded.size()) + tr(zh, " 字符", " chars");

    const std::string preview = v.value + (expandable ? "\n\n" + expanded : "");
    const std::string icon = isUser ? "color:#185FA5" : "color:#8A8A8A";

    // Inline edit covers the single-line grammar only: PATH-style separator
    // items, multi-string and any value the launcher grammar cannot round-trip
    // go through the editor, which keeps element boundaries and types.
    const bool inlineEditable = (v.type == "REG_SZ" || v.type == "REG_EXPAND_SZ")
                                && v.value.find('\n') == std::string::npos
                                && v.value.find(';') == std::string::npos;

    std::vector<winshun::Action> actions {
        {"open", tr(zh, "打开编辑器", "Open editor"), "Enter"},
        {"copyValue", tr(zh, "复制原始值", "Copy value"), "Ctrl+Enter"}};
    if (expandable)
        actions.push_back({"copyExpanded", tr(zh, "复制展开值", "Copy expanded"), "Shift+Enter"});
    actions.push_back({"copyName", tr(zh, "复制变量名", "Copy name"), ""});
    actions.push_back({"copyAssign", tr(zh, "复制 NAME=value", "Copy NAME=value"), ""});
    if (inlineEditable)
        actions.push_back({"inlineEdit", tr(zh, "行内编辑", "Quick edit"), ""});
    actions.push_back({"delete", tr(zh, "删除", "Delete"), ""});

    winshun::Item item(envh::envId(v), title, subtitle);
    item.withIcon(icon)
        .withScore(isUser ? 95 : 50)
        .withCopy(v.value)
        .withPreview(preview)
        .withData(rowData({{"kind", "env"},
                          {"scope", v.scope},
                          {"name", v.name},
                          {"value", v.value},
                          {"type", v.type},
                          {"select", v.scope + "/" + v.name},
                          {"primary", v.value},
                          {"detail", expanded},
                          {"chain", "env " + v.name},
                          {"expand", expandable}}))
        .withActions(std::move(actions));
    return item;
}

// ---- hosts rows ----------------------------------------------------------------

winshun::Item hostsEntryRow(const envh::HostsEntry& e, const std::string& id, bool writable, bool zh)
{
    const std::string raw = e.raw;
    if (e.section) {
        // A divider is not a record: nothing to enable, toggle or delete, just
        // the text itself.
        winshun::Item item(id, e.comment, tr(zh, "分组注释", "section comment"));
        item.invalid()
            .withIcon("color:#8A8A8A")
            .withScore(10)
            .withCopy(raw)
            .withPreview(raw)
            .withData(rowData({{"kind", "hostsSection"}, {"primary", raw}, {"detail", raw}, {"chain", ""}, {"expand", false}}));
        return item;
    }
    const std::string title = (e.commented ? tr(zh, "已注释 · ", "commented · ") : "") + e.ip + "  " + e.hostname;
    std::string subtitle = tr(zh, "第 ", "line ") + std::to_string(e.lineIndex + 1)
                           + tr(zh, " 行 · ", " · ") + (e.commented ? tr(zh, "已注释", "commented") : tr(zh, "生效", "active"));
    if (!writable)
        subtitle += tr(zh, " · 🔒 需管理员", " · 🔒 needs admin");
    const std::string icon = e.commented ? "color:#8A8A8A" : "color:#107C10";
    // The default action is named after its outcome: enable a commented line,
    // disable a live one.
    winshun::Item item(id, title, subtitle);
    item.withIcon(icon)
        .withScore(e.commented ? 60 : 90)
        .withCopy(raw)
        .withPreview(raw)
        .withData(rowData({{"kind", "hosts"},
                          {"id", id},
                          {"lineIndex", e.lineIndex},
                          {"ip", e.ip},
                          {"hostname", e.hostname},
                          {"commented", e.commented},
                          {"select", std::to_string(e.lineIndex + 1)},
                          {"primary", raw},
                          {"detail", raw},
                          {"chain", "hosts " + e.hostname},
                          {"expand", false}}))
        .withActions(std::vector<winshun::Action> {
            {e.commented ? "enable" : "disable", e.commented ? tr(zh, "启用", "Enable") : tr(zh, "禁用", "Disable"), "Enter"},
            {"edit", tr(zh, "编辑并定位", "Edit and locate"), ""},
            {"copyIp", tr(zh, "复制 IP", "Copy IP"), ""},
            {"copyHostname", tr(zh, "复制主机名", "Copy hostname"), ""},
            {"copyLine", tr(zh, "复制整行", "Copy line"), "Ctrl+Enter"},
            {"openFile", tr(zh, "打开 hosts 文件", "Open hosts file"), "Shift+Enter"},
            {"delete", tr(zh, "删除该行", "Delete line"), ""}});
    return item;
}

// ---- edit / confirm rows -------------------------------------------------------

winshun::Item envEditRow(const std::string& name, const std::string& value, const std::string& scope,
                         bool valid, const std::string& errMsg, bool zh)
{
    const std::string title = (name.empty() ? tr(zh, "新建变量", "new variable") : tr(zh, "保存 ", "save ") + name)
                             + " = " + (value.size() > 80 ? value.substr(0, 80) + "…" : value);
    const std::string subtitle = tr(zh, "Enter 保存 · Ctrl+Enter 取消 · Shift+Enter 复制",
                                    "Enter save · Ctrl+Enter cancel · Shift+Enter copy")
                                 + (valid ? "" : " · " + errMsg);
    winshun::Item item("env:edit:" + (name.empty() ? "new" : envh::toLower(name)), title, subtitle);
    item.withIcon("color:#185FA5")
        .withScore(100)
        .withCopy(value)
        .withPreview(name + " = " + value)
        .withData(rowData({{"kind", "envEdit"},
                          {"scope", scope},
                          {"name", name},
                          {"value", value},
                          {"primary", value},
                          {"detail", name + " = " + value},
                          {"chain", "env " + name},
                          {"expand", false}}));
    if (!valid)
        item.invalid();
    item.withActions(std::vector<winshun::Action> {
        {"save", tr(zh, "保存", "Save"), "Enter"},
        {"cancel", tr(zh, "取消", "Cancel"), "Ctrl+Enter"},
        {"copy", tr(zh, "复制", "Copy"), "Shift+Enter"}});
    return item;
}

winshun::Item confirmRow(const std::string& id, const std::string& kind, const std::string& target,
                         const std::string& title, const std::string& subtitle, bool zh,
                         winshun::Json extra = winshun::Json(winshun::Json::Object {}))
{
    winshun::Json::Object data {{"kind", kind}, {"target", target}, {"primary", target}, {"detail", subtitle}, {"chain", ""}, {"expand", false}};
    for (const auto& [key, value] : extra.toObject())
        data[key] = value;
    winshun::Item item(id, title, subtitle);
    item.invalid()
        .withIcon("color:#C0392B")
        .withScore(100)
        .withCopy(target)
        .withData(winshun::Json(std::move(data)))
        .withActions(std::vector<winshun::Action> {
            {"confirm", tr(zh, "确认", "Confirm"), "Enter"},
            {"cancel", tr(zh, "取消", "Cancel"), "Ctrl+Enter"}});
    return item;
}

// ---- pinned "open interactive editor" row (always the first result) -----------

winshun::Item editorEntryRow(const std::string& kind, const std::string& filter, bool zh)
{
    const bool isEnv = kind == "env";
    winshun::Item item(kind + ":editor", tr(zh, "进入交互式编辑界面", "Open interactive editor"),
                      (isEnv ? tr(zh, "可视化编辑全部环境变量", "Visually edit all environment variables")
                             : tr(zh, "可视化编辑 hosts 文件", "Visually edit the hosts file"))
                          + tr(zh, " · Enter 打开独立窗口", " · Enter opens a standalone window"));
    item.withIcon(isEnv ? "color:#185FA5" : "color:#107C10")
        .withScore(1000)
        .withPreview(isEnv
                         ? tr(zh, "打开独立窗口：纯键盘操作、撤销/重做、冲突检测、草稿自动保存",
                              "Standalone window: full keyboard control, undo/redo, conflict detection, auto-saved drafts")
                         : tr(zh, "打开独立窗口：注释/启用、增删行、备份与冲突检测",
                              "Standalone window: toggle/enable, add/remove lines, backups, conflict detection"))
        .withData(rowData({{"kind", "openEditor"},
                          {"target", kind},
                          {"filter", filter},
                          {"select", ""},
                          {"primary", tr(zh, "进入交互式编辑界面", "Open interactive editor")},
                          {"detail", ""},
                          {"chain", ""},
                          {"expand", false}}))
        .withActions(std::vector<winshun::Action> {{"open", tr(zh, "打开", "Open"), "Enter"}});
    return item;
}

#ifdef _WIN32
// ---- resident plugin -----------------------------------------------------------

struct EnvHosts : winshun::Plugin {
    std::string name() const override { return "ra9fael.envhosts"; }

    // On shutdown (serve() returns) close any open editor windows and join their
    // GUI threads so unsaved edits are written as drafts and no std::thread is left
    // joinable (which would terminate the process).
    ~EnvHosts() override
    {
#ifdef _WIN32
        editorwin::closeAllEditors(1500);
#endif
    }

    // Test-only escape: when these env vars are set the plugin targets a scratch
    // key / file instead of the real Environment / hosts, so integration tests
    // can run without touching the real system. Production never sets them.
    std::wstring testUserSubkey()
    {
        const std::string s = winshun::env("WS_ENVHOSTS_TEST_SUBKEY");
        return s.empty() ? std::wstring(L"Environment") : winshun::wide(s);
    }
    std::wstring testSystemSubkey()
    {
        const std::string s = winshun::env("WS_ENVHOSTS_TEST_SYSTEM_SUBKEY");
        return s.empty() ? envh::native::systemEnvSubkey() : winshun::wide(s);
    }
    std::string testHostsPath(const Settings& st)
    {
        const std::string s = winshun::env("WS_ENVHOSTS_TEST_HOSTS");
        return s.empty() ? expandPathVars(st.hostsPath) : s;
    }

    std::wstring sshDirPath(const Settings& st)
    {
        const std::string test = winshun::env("WS_ENVHOSTS_TEST_SSH");
        if (!test.empty()) return winshun::wide(test);
        return st.sshDir.empty() ? envh::native::defaultSshDir() : winshun::wide(expandPathVars(st.sshDir));
    }

    // Host locale (zh / en) plus the plugin's own override. Everything visible
    // is resolved from this at query time, so a language change needs no restart.
    envh::i18n::Lang uiLang() const
    {
        return envh::i18n::resolve(locale(), readSettings(settings()).uiLanguage);
    }
    bool uiChinese() const { return envh::i18n::chinese(uiLang()); }

    // Shared OpenArgs construction for the pinned editor row and the per-item
    // "open and locate" actions, so both open the same window the same way.
    // `select` carries the target: "user/NAME" / "system/NAME" for env, the
    // 1-based hosts line number for hosts.
    editorwin::OpenArgs buildEditorArgs(const std::string& kind, const std::string& filter,
                                        const std::string& select, const Settings& st)
    {
        editorwin::OpenArgs oa;
        oa.kind = kind;
        oa.userSubkey = testUserSubkey();
        oa.sysSubkey = testSystemSubkey();
        oa.hostsPath = winshun::wide(testHostsPath(st));
        oa.sshDir = sshDirPath(st);
        oa.dataDir = dataDir();
        oa.backupCount = st.backupCount;
        oa.showSystem = st.showSystem;
        oa.filter = filter;
        oa.select = select;
        oa.setLang(uiLang());
        oa.dark = (theme() == "dark");
        oa.appearanceJson = appearance().dump();
        oa.elevated = envh::native::processElevated();
        if (kind == "hosts") {
            envh::native::HostsStore hs(winshun::wide(testHostsPath(st)));
            std::string werr;
            oa.writable = hs.isWritable(werr);
        } else {
            oa.writable = true;
            // System scope is writable only with an elevated token (the
            // plugin declares `elevated` and Win顺 runs as administrator).
            envh::native::EnvStore es(oa.userSubkey, oa.sysSubkey);
            std::string werr;
            oa.systemWritable = st.showSystem && es.canWriteSystem(werr);
        }
        return oa;
    }

    winshun::Effects openEditorTarget(const std::string& kind, const std::string& filter,
                                      const std::string& select, const Settings& st, bool zh)
    {
        const editorwin::OpenArgs oa = buildEditorArgs(kind, filter, select, st);
        std::string err;
#ifdef ENVHOSTS_QML_EDITOR
        // Do not block the action reply on QML component creation. The
        // protocol thread answers while the GUI thread creates the window.
        if (editorwin::openEditorDeferred(oa))
            return {winshun::effect::notify(tr(zh, "正在打开编辑窗口", "Opening editor window")), winshun::effect::hide()};
        err = "E_QML_INIT";
#else
        if (editorwin::openEditor(oa, err))
            return {winshun::effect::notify(tr(zh, "已打开编辑窗口", "Editor window opened")), winshun::effect::hide()};
#endif
        return {winshun::effect::notify(tr(zh, "无法打开编辑窗口：", "Cannot open editor: ") + err), winshun::effect::keep()};
    }

    winshun::Items query(const winshun::Query& q) override
    {
        const bool zh = uiChinese();
        const Settings st = readSettings(settings());
        std::string command = q.command;
        std::string text = winshun::trim(q.text);
        if (command != "env" && command != "hosts" && command != "sshm" && command != "ssh") {
            const std::string raw = winshun::trim(q.raw);
            for (const char* kw : {"env", "hosts", "sshm", "ssh"}) {
                const std::string prefix = std::string(kw) + " ";
                const std::string cprefix = std::string(kw) + ":";
                if (envh::startsWith(raw, prefix)) {
                    command = kw;
                    text = winshun::trim(raw.substr(prefix.size()));
                    break;
                }
                if (envh::startsWith(raw, cprefix)) {
                    command = kw;
                    text = winshun::trim(raw.substr(cprefix.size()));
                    break;
                }
                if (raw == kw) {
                    command = kw;
                    text.clear();
                    break;
                }
            }
        }
        // The host strips the keyword but leaves the grammar's leading ':' for the
        // inline-edit / sub-command forms (e.g. "env:set NAME=v" -> text ":set NAME=v").
        if (!text.empty() && text[0] == ':')
            text = winshun::trim(text.substr(1));
        if (command == "sshm" || command == "ssh") {
            const bool keysMode = text == "keys" || envh::startsWith(text, "keys ");
            const std::string filter = keysMode ? winshun::trim(text.substr(4)) : text;
            winshun::Items items;
            editorwin::OpenArgs oa; oa.kind = "ssh"; oa.sshDir = sshDirPath(st); oa.dataDir = dataDir(); oa.backupCount = st.backupCount; oa.filter = filter; oa.select = keysMode ? "key:" : ""; oa.setLang(uiLang()); oa.dark = theme() == "dark";
            winshun::Item open("sshm:editor", tr(zh, "进入 SSH 管理器", "Open SSH manager"), tr(zh, "管理 SSH 客户端配置、密钥与 known_hosts", "Manage SSH config, keys and known_hosts"));
            open.withScore(1000).withData(rowData({{"kind","openSshEditor"},{"filter",filter},{"mode",keysMode?"keys":"conn"},{"primary","SSH"},{"detail",""},{"chain",""},{"expand",false}})).withActions({{"open",tr(zh,"打开","Open"),"Enter"}}); items.push_back(open);
            envh::native::SshStore store(oa.sshDir, oa.sshDir + L"\\config"); const auto read = store.read();
            if (keysMode) { const auto keys = store.keys(); int shown=0; for(size_t i=0;i<keys.size()&&shown<st.maxResults;++i) if(envh::ssh::keyMatches(keys[i],filter)){ const auto& k=keys[i]; winshun::Item row("sshm:key:"+std::to_string(i),k.name,tr(zh,"SSH 密钥","SSH key")); row.withData(rowData({{"kind","sshKey"},{"name",k.name},{"pub",k.pub.ok?k.pub.algo+" "+envh::ssh::base64(k.pub.blob):""},{"primary",k.name},{"detail",""},{"chain","sshm:keys "+k.name},{"expand",false}})).withActions({{"open",tr(zh,"打开","Open"),"Enter"}});items.push_back(row);++shown;} }
            else if (read.ok && !read.missing) { const auto cfg=envh::ssh::parseConfig(read.content); int shown=0; for(int i=0;i<static_cast<int>(cfg.connectionIndexes().size())&&shown<st.maxResults;++i){const auto* b=cfg.connection(i);if(!b||!envh::ssh::connMatches(*b,filter))continue;const auto name=envh::ssh::displayName(*b);winshun::Item row("sshm:conn:"+std::to_string(i),name,b->fields.hostname);row.withData(rowData({{"kind","sshConn"},{"alias",name},{"locked",envh::ssh::isLocked(*b)},{"text",envh::ssh::trim(envh::ssh::serializeBlock(*b,cfg.eol()))},{"primary",name},{"detail",b->fields.hostname},{"chain","sshm "+name},{"expand",false}})).withActions({{"edit",tr(zh,"编辑","Edit"),"Enter"},{"copySshCommand",tr(zh,"复制 SSH 命令","Copy SSH command"),""},{"openTerminal",tr(zh,"用 Windows Terminal 打开","Open in Windows Terminal"),""},{"motty",tr(zh,"用 Motty 连接","Connect with Motty"),""},{"mottyAlias",tr(zh,"用 Motty 按 alias 连接","Connect with Motty by alias"),""},{"delete",tr(zh,"删除","Delete"),""}});items.push_back(row);++shown;} }
            return items;
        }
        envh::ParsedQuery pq = envh::parseQuery(command, text);
        if (pq.hasError)
            return {errorRow("error." + pq.errorCode, pq.errorCode, tr(zh, "输入有误", "Invalid input") + " · " + pq.errorCode,
                             tr(zh, "示例：", "Example: ") + pq.errorHint, pq.errorHint, zh)};

        if (command == "env")
            return envQuery(pq, st, zh);
        if (command == "hosts")
            return hostsQuery(pq, st, zh);
        return {errorRow("error.unk", "E_COMMAND", tr(zh, "未知命令", "Unknown command"), "env | hosts", "env | hosts", zh)};
    }

    winshun::Items envQuery(const envh::ParsedQuery& pq, const Settings& st, bool zh)
    {
        envh::native::EnvStore store(testUserSubkey(), testSystemSubkey());
        const envh::native::EnvReadResult r = store.read(st.showSystem);
        const std::map<std::string, std::string> lookup = envh::envLookup(r.snap);
        // Elevated plugin (Plan A): system-scope rows are writable too, so the
        // per-row lock hint must follow a real probe, not the scope name.
        std::string sysWerr;
        const bool sysWritable = store.canWriteSystem(sysWerr);

        if (pq.kind == envh::CmdKind::EnvSet || pq.kind == envh::CmdKind::EnvNew) {
            const std::string name = pq.kind == envh::CmdKind::EnvNew ? "" : pq.name;
            const std::string value = pq.kind == envh::CmdKind::EnvNew ? "" : pq.value;
            const bool valid = !name.empty() && envh::isValidEnvName(name) && envh::isValidEnvValue(value);
            std::string err;
            if (name.empty() && pq.kind == envh::CmdKind::EnvNew)
                err = tr(zh, "在等号的左侧输入变量名", "type the name before '='");
            else if (!envh::isValidEnvName(name))
                err = tr(zh, "变量名需 1-255 字符且不含 '=' 或控制字符", "name needs 1-255 chars, no '=' or control chars");
            else if (!envh::isValidEnvValue(value))
                err = tr(zh, "值过长或含有控制字符", "value too long or has control chars");
            // Editing an existing variable keeps its scope (and, through the
            // matched `before`, its registry type); a new variable keeps the
            // command's default scope. The save path picks the write target
            // from this scope, so a system variable can never land in user.
            std::string scope = "user";
            if (!name.empty() && envh::isValidEnvName(name)) {
                const envh::EnvVar* existing = envh::findEnvVar(r.snap, name, "user");
                if (existing == nullptr && st.showSystem)
                    existing = envh::findEnvVar(r.snap, name, "system");
                if (existing != nullptr)
                    scope = existing->scope;
            }
            return {envEditRow(name, value, scope, valid, err, zh)};
        }

        if (pq.kind == envh::CmdKind::EnvRestore) {
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            winshun::Items items;
            for (const auto& b : backups.listEnv()) {
                winshun::Item item("env:backup:" + b.id, tr(zh, "恢复备份 ", "restore backup ") + b.id,
                                   tr(zh, "Enter 恢复用户变量", "Enter restores user vars"));
                item.withIcon("color:#8A8A8A")
                    .withScore(80)
                    .withData(rowData({{"kind", "envRestore"}, {"backupId", b.id}, {"primary", b.id}, {"detail", b.id}, {"chain", ""}, {"expand", false}}))
                    .withActions(std::vector<winshun::Action> {{"restore", tr(zh, "恢复", "Restore"), "Enter"}});
                items.push_back(std::move(item));
            }
            if (items.empty())
                items.push_back(errorRow("env:backup:none", "E_NOBACKUP", tr(zh, "还没有备份", "No backups yet"),
                                         tr(zh, "先编辑一个用户变量", "edit a user variable first"), "", zh));
            return items;
        }

        // EnvList
        winshun::Items items;
        items.push_back(editorEntryRow("env", pq.filter, zh));
        if (pq.filter.empty()) {
            int userN = 0, sysN = 0;
            for (const auto& v : r.snap.vars)
                (v.scope == "user") ? ++userN : ++sysN;
            const std::string hint = tr(zh, "用户变量 ", "User vars ") + std::to_string(userN)
                                     + tr(zh, " 项 · 系统变量 ", " · System vars ") + std::to_string(sysN)
                                     + tr(zh, " 项 · 输入更具体的名字可过滤", " · type a name to filter");
            winshun::Item h("env:hint", tr(zh, "环境变量", "Environment variables"), hint);
            h.invalid()
                .withIcon("color:#185FA5")
                .withScore(0)
                .withData(rowData({{"kind", "hint"}, {"primary", ""}, {"detail", hint}, {"chain", ""}, {"expand", false}}));
            items.push_back(std::move(h));
        }
        for (const auto& v : r.snap.vars) {
            if (!envh::envMatches(v, pq.filter))
                continue;
            const std::string expanded = v.type == "REG_EXPAND_SZ"
                                             ? envh::expandEnvValue(v.value, lookup)
                                             : v.value;
            items.push_back(envVarRow(v, expanded, /*writableScope=*/v.scope == "user" || sysWritable, zh));
            if (static_cast<int>(items.size()) >= st.maxResults)
                break;
        }
        if (items.size() == (pq.filter.empty() ? 1u : 0u))
            items.push_back(errorRow("env:empty", "E_EMPTY", tr(zh, "没有匹配的环境变量", "No matching environment variable"),
                                     tr(zh, "试试 env path", "try env path"), "", zh));
        return items;
    }

    winshun::Items hostsQuery(const envh::ParsedQuery& pq, const Settings& st, bool zh)
    {
        const std::string path = testHostsPath(st);
        envh::native::HostsStore store(winshun::wide(path));
        const envh::native::HostsReadResult r = store.read();
        if (!r.ok) {
            if (r.utf16)
                return {errorRow("hosts:utf16", "E_HOSTS_UTF16", tr(zh, "hosts 是 UTF-16", "hosts is UTF-16"),
                                 tr(zh, "请先转成 UTF-8 再编辑", "convert to UTF-8 first"), r.errMsg, zh)};
            return {errorRow("hosts:read", r.errCode, tr(zh, "hosts 读不到", "Cannot read hosts"),
                             r.errMsg, r.errMsg, zh)};
        }
        std::string writableErr;
        const bool writable = store.isWritable(writableErr);

        if (pq.kind == envh::CmdKind::HostsAdd) {
            const auto add = envh::hostsAdd(envh::parseHosts(r.content), pq.ip, pq.hostname, pq.comment);
            if (!add.success)
                return {errorRow("hosts:add", add.failure.code, tr(zh, "无法添加", "Cannot add"), add.failure.hint,
                                 add.failure.message, zh)};
            const std::string target = pq.ip + " " + pq.hostname + (pq.comment.empty() ? "" : " # " + pq.comment);
            return {confirmRow("hosts:add:" + envh::toLower(pq.hostname), "hostsAddConfirm", target,
                               tr(zh, "添加 ", "add ") + target,
                               tr(zh, "Enter 确认写入 hosts", "Enter writes to hosts") + (!writable ? tr(zh, "（需管理员）", " (needs admin)") : ""), zh)};
        }

        if (pq.kind == envh::CmdKind::HostsRestore) {
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            winshun::Items items;
            for (const auto& b : backups.listHosts()) {
                winshun::Item item("hosts:backup:" + b.id, tr(zh, "恢复备份 ", "restore backup ") + b.id,
                                   tr(zh, "Enter 恢复 hosts", "Enter restores hosts"));
                item.withIcon("color:#8A8A8A")
                    .withScore(80)
                    .withData(rowData({{"kind", "hostsRestore"}, {"backupId", b.id}, {"primary", b.id}, {"detail", b.id}, {"chain", ""}, {"expand", false}}))
                    .withActions(std::vector<winshun::Action> {{"restore", tr(zh, "恢复", "Restore"), "Enter"}});
                items.push_back(std::move(item));
            }
            if (items.empty())
                items.push_back(errorRow("hosts:backup:none", "E_NOBACKUP", tr(zh, "还没有备份", "No backups yet"),
                                         tr(zh, "先切换一条 hosts 记录", "toggle a hosts entry first"), "", zh));
            return items;
        }

        // HostsList
        const envh::HostsParsed parsed = envh::parseHosts(r.content);
        const std::vector<envh::HostsEntry> entries = envh::hostsEntries(parsed);
        winshun::Items items;
        items.push_back(editorEntryRow("hosts", pq.filter, zh));
        if (pq.filter.empty()) {
            winshun::Item h("hosts:hint", tr(zh, "hosts 文件", "hosts file"),
                            tr(zh, "共 ", "entries: ") + std::to_string(entries.size())
                                + tr(zh, " 条 · 输入可过滤 IP / 主机名 / 注释", " entries · type to filter")
                                + (!writable ? tr(zh, " · 🔒 只读（需管理员写入）", " · 🔒 read-only (admin to write)") : ""));
            h.invalid()
                .withIcon("color:#107C10")
                .withScore(0)
                .withData(rowData({{"kind", "hint"}, {"primary", ""}, {"detail", ""}, {"chain", ""}, {"expand", false}}));
            items.push_back(std::move(h));
        }
        // Track id collisions so identical lines get a "-2" suffix (stable ids).
        std::map<std::string, int> seen;
        for (const auto& e : entries) {
            if (!envh::hostsMatches(e, pq.filter))
                continue;
            std::string base = envh::hostsEntryId(e);
            const int n = seen[base]++;
            const std::string id = base + (n == 0 ? "" : "-" + std::to_string(n + 1));
            items.push_back(hostsEntryRow(e, id, writable, zh));
            if (static_cast<int>(items.size()) >= st.maxResults)
                break;
        }
        if (items.size() == (pq.filter.empty() ? 1u : 0u))
            items.push_back(errorRow("hosts:empty", "E_EMPTY", tr(zh, "没有匹配的 hosts 条目", "No matching hosts entry"),
                                     tr(zh, "试试 hosts docker", "try hosts docker"), "", zh));
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const bool zh = uiChinese();
        const Settings st = readSettings(settings());
        const winshun::Json data = a.item["data"];
        const std::string kind = data["kind"].toString();
        const auto notify = [&](const std::string& t) { return winshun::effect::notify(t); };

        // ---- open the interactive editor window ----
        if (kind == "openEditor") {
            const std::string target = data["target"].toString(); // "env" | "hosts"
            return openEditorTarget(target, data["filter"].toString(), data["select"].toString(), st, zh);
        }

        if (kind == "openSshEditor")
            return openEditorTarget("ssh", data["filter"].toString(), data["mode"].toString() == "keys" ? "key:" : "", st, zh);

        if (kind == "sshConn") {
            const std::wstring dir = sshDirPath(st);
            envh::native::SshStore store(dir, dir + L"\\config");
            const auto disk = store.read();
            if (!disk.ok) return {notify(disk.errMsg), winshun::effect::keep()};
            const auto cfg = disk.missing ? envh::ssh::Config::empty() : envh::ssh::parseConfig(disk.content);
            const std::string alias = data["alias"].toString();
            const envh::ssh::Block* block = nullptr;
            for (int i=0;i<static_cast<int>(cfg.connectionIndexes().size());++i) if (const auto* b=cfg.connection(i); b && envh::ssh::displayName(*b)==alias) { block=b; break; }
            if (!block) return {notify(tr(zh,"找不到该连接","Connection not found")), winshun::effect::keep()};
            if (envh::ssh::isLocked(*block))
                return {notify(tr(zh,"该块只读，不能执行此动作","This block is read-only; the action is unavailable")), winshun::effect::keep()};
            if (a.action == "edit") return openEditorTarget("ssh", "", alias, st, zh);
            if (a.action == "copySshCommand") return {winshun::effect::copy("ssh.exe -F \""+winshun::utf8(dir+L"\\config")+"\" \""+alias+"\""), notify(tr(zh,"已复制 SSH 命令","SSH command copied")), winshun::effect::keep()};
            if (a.action == "mottyAlias") return {notify(tr(zh,"Motty 当前不支持按 OpenSSH alias 直连","Motty does not support direct OpenSSH alias connections yet")), winshun::effect::keep()};
            if (a.action == "openTerminal") { const auto wt=envh::native::findProgram(L"wt.exe"); const auto ssh=envh::native::findProgram(L"ssh.exe"); if(wt.empty()||ssh.empty())return {notify(tr(zh,"找不到 Windows Terminal 或 ssh.exe","Windows Terminal or ssh.exe was not found")),winshun::effect::keep()}; const auto launch=envh::native::launchProcess(wt,{L"new-tab",ssh,L"-F",dir+L"\\config",envh::native::toWide(alias)}); return launch.launched?winshun::Effects{notify(tr(zh,"已在 Windows Terminal 中打开","Opened in Windows Terminal")),winshun::effect::hide()}:winshun::Effects{notify(launch.err),winshun::effect::keep()}; }
            if (a.action == "motty") { const std::string host=envh::ssh::trim(block->fields.hostname); const std::string user=envh::ssh::trim(block->fields.user); const std::string port=envh::ssh::trim(block->fields.port).empty()?"22":envh::ssh::trim(block->fields.port); const std::string url="ssh://"+(user.empty()?"":user+"@")+host+":"+port; std::string error; if(!envh::native::openUrl(winshun::wide(url),error))return {notify(error),winshun::effect::keep()}; return {notify(tr(zh,"已请求 Motty 按地址连接","Motty was asked to connect by address")),winshun::effect::hide()}; }
            if (a.action == "delete") {
                return {winshun::effect::replace(winshun::Items {confirmRow("sshm:del:" + envh::toLower(alias), "sshDeleteConfirm", alias,
                                                                           tr(zh, "删除 ", "Delete ") + alias,
                                                                           tr(zh, "Enter 确认删除，Ctrl+Enter 取消", "Enter confirms deletion, Ctrl+Enter cancels"), zh)}),
                        winshun::effect::keep()};
            }
        }

        if (kind == "sshDeleteConfirm") {
            if (a.action == "cancel") return {winshun::effect::setQuery("sshm"), winshun::effect::keep()};
            const std::wstring dir = sshDirPath(st); envh::native::SshStore store(dir, dir + L"\\config"); const auto disk = store.read();
            if (!disk.ok || disk.missing) return {notify(tr(zh,"SSH 配置读不到","Cannot read the SSH config")), winshun::effect::keep()};
            auto cfg = envh::ssh::parseConfig(disk.content); const std::string alias = data["target"].toString(); int target = -1;
            for (int i=0;i<static_cast<int>(cfg.connectionIndexes().size());++i) if (const auto* b=cfg.connection(i); b && !envh::ssh::isLocked(*b) && envh::ssh::displayName(*b)==alias) { target=i; break; }
            if (target < 0) return {notify(tr(zh,"找不到该连接","Connection not found")), winshun::effect::keep()};
            envh::native::BackupStore backups(dataDir(), st.backupCount); backups.saveSsh(disk.content); if (!cfg.toggleDelete(target)) return {notify(tr(zh,"删除失败","Delete failed")), winshun::effect::keep()};
            std::string error; if (!store.write(cfg.serialize(), error)) return {notify(error), winshun::effect::keep()}; editorwin::onExternalChange("ssh");
            return {notify(tr(zh,"已删除 ","Deleted ") + alias), winshun::effect::setQuery("sshm"), winshun::effect::hide()};
        }

        // ---- env actions ----
        if (kind == "env") {
            const std::string scope = data["scope"].toString();
            const std::string name = data["name"].toString();
            const std::string value = data["value"].toString();
            if (a.action == "open")
                return openEditorTarget("env", name, scope + "/" + name, st, zh);
            if (a.action == "inlineEdit") {
                // Replace the list with the edit row directly: routing through
                // env:set would re-resolve the scope and could pick the user
                // variable when both scopes carry the same name.
                return {winshun::effect::replace(winshun::Items {envEditRow(name, value, scope, /*valid=*/true, "", zh)}),
                        winshun::effect::keep()};
            }
            if (a.action == "copyValue")
                return {winshun::effect::copy(value), notify(tr(zh, "已复制值", "Copied value")), winshun::effect::hide()};
            if (a.action == "copyExpanded")
                return {winshun::effect::copy(data["detail"].toString()),
                        notify(tr(zh, "已复制展开值", "Copied expanded")), winshun::effect::hide()};
            if (a.action == "copyName")
                return {winshun::effect::copy(name), notify(tr(zh, "已复制变量名", "Copied name")), winshun::effect::hide()};
            if (a.action == "copyAssign")
                return {winshun::effect::copy(name + "=" + value), notify(tr(zh, "已复制 NAME=value", "Copied NAME=value")), winshun::effect::hide()};
            if (a.action == "delete") {
                // System scope is allowed through to the confirm row; the
                // shared write transaction below re-probes and refuses only
                // when the write would actually fail.
                const std::string target = scope + "/" + name;
                const bool well = envh::isWellKnownName(name);
                const std::string sub = tr(zh, "确认删除 ", "Confirm delete ") + target
                                         + (well ? tr(zh, "（重要变量，谨慎）", " (important var, be careful)") : "")
                                         + tr(zh, "？Enter 确认，Ctrl+Enter 取消", "? Enter confirms, Ctrl+Enter cancels");
                return {winshun::effect::replace(winshun::Items {confirmRow("env:del:" + envh::toLower(name), "envDeleteConfirm", target,
                                                                         tr(zh, "删除 ", "delete ") + target, sub, zh)}),
                        winshun::effect::keep()};
            }
            return {notify(tr(zh, "未支持的动作", "Unsupported action")), winshun::effect::hide()};
        }

        // ---- env inline edit ----
#ifdef ENVHOSTS_QML_EDITOR
        // The launcher and QML window share the same verified write transaction.
        // Opening a window is deliberately outside the process-wide write lock.
        const bool inlineWrite=(kind=="envEdit"&&a.action=="save")
            ||(kind=="envDeleteConfirm"&&a.action!="cancel")
            ||(kind=="hosts"&&a.action=="toggle")
            ||((kind=="hostsDeleteConfirm"||kind=="hostsAddConfirm")&&a.action!="cancel");
        if(inlineWrite){
            const bool environment=kind=="envEdit"||kind=="envDeleteConfirm";
            editorwin::OpenArgs args;args.kind=environment?"env":"hosts";args.userSubkey=testUserSubkey();args.sysSubkey=testSystemSubkey();
            args.hostsPath=winshun::wide(testHostsPath(st));args.dataDir=dataDir();args.backupCount=st.backupCount;args.showSystem=true;
            editorwin::NativeBackend backend(args);auto snapshot=backend.read();
            if(!snapshot.error.isEmpty())return {notify(snapshot.error.toStdString()),winshun::effect::keep()};
            editorwin::WriteRequest request;request.kind=QString::fromStdString(args.kind);request.token=editorwin::freshId();request.revision="launcher-action";
            if(environment){
                // The edit row carries the scope it was opened with (the scope
                // of the existing variable, or the new-variable default); the
                // delete confirm carries it as the "scope/NAME" target.
                const auto target=data["target"].toString();
                const QString scope=QString::fromStdString(kind=="envEdit"?data["scope"].toString():(target.compare(0,7,"system/")==0?"system":"user"));
                if(scope!="user"&&scope!="system")return {notify("E_SCOPE"),winshun::effect::keep()};
                const QString name=QString::fromStdString(kind=="envEdit"?data["name"].toString():target.substr(target.find('/')+1));
                if(!envh::isValidEnvName(name.toStdString()))return {notify("E_ENV_NAME"),winshun::effect::keep()};
                editorwin::Change change;change.scope=scope;change.name=name;
                for(const auto& value:snapshot.env)if(value.key()==editorwin::envKey(scope,name)){change.before=value;change.hasBefore=true;}
                change.hasAfter=kind=="envEdit";
                if(change.hasAfter){const auto value=data["value"].toString();if(!envh::isValidEnvValue(value))return {notify("E_ENV_VALUE"),winshun::effect::keep()};
                    if(change.hasBefore&&(change.before.type=="REG_MULTI_SZ"||!change.before.formatError.isEmpty()))return {notify(tr(zh,"请在 QML 编辑器中编辑此类型，避免丢失元素边界。","Edit this type in the QML editor to preserve element boundaries.")),winshun::effect::keep()};
                    change.after=change.hasBefore?change.before:editorwin::EnvValue{};change.after.scope=scope;change.after.name=name;
                    if(!change.hasBefore)change.after.type=QString::fromStdString(envh::envTypeForValue(value));
                    change.after.value=QString::fromStdString(value);change.after.encode();}
                change.operation=change.hasAfter?(change.hasBefore?"modify":"add"):"delete";
                if(change.hasAfter&&change.hasBefore&&editorwin::sameValue(change.before,change.after))return {notify(tr(zh,"没有修改","No changes")),winshun::effect::keep()};
                request.changes.append(change);request.fingerprints[scope]=snapshot.fingerprint(scope);
            }else{
                auto parsed=envh::parseHosts(snapshot.hosts.toStdString());
                if(kind=="hostsAddConfirm"){
                    const auto target=data["target"].toString();auto row=envh::parseHostLine(target,0);
                    if(row.section||row.hostnames.empty())return {notify("E_HOSTS_HOST"),winshun::effect::keep()};
                    auto added=envh::hostsAdd(parsed,row.ip,row.hostnames.front(),row.comment);if(!added.success)return {notify(added.failure.message),winshun::effect::keep()};parsed=added.value;
                }else{
                    // Resolve by the identity carried from the queried row
                    // (stable id with its occurrence), then compare the raw
                    // line: a record that changed after the query refreshes
                    // instead of writing to whichever line matches now.
                    int line=-1;
                    if(!envh::findHostsEntryById(parsed,data["id"].toString(),line)
                       || parsed.lines[size_t(line)].text!=data["primary"].toString())
                        return {notify(tr(zh,"该行已被外部修改，列表已刷新","The line changed externally; the list was refreshed")),
                                winshun::effect::setQuery("hosts "+data["hostname"].toString()),winshun::effect::keep()};
                    auto changed=kind=="hosts"?envh::hostsToggle(parsed,line):envh::hostsDelete(parsed,line);
                    if(!changed.success)return {notify(changed.failure.message),winshun::effect::keep()};parsed=changed.value;
                }
                request.hosts=QByteArray::fromStdString(envh::serializeHosts(parsed));request.fingerprints["hosts"]=snapshot.fingerprint("hosts");
            }
            const auto result=editorwin::WriteService::execute(backend,request);
            if(result.state!="Succeeded"&&result.state!="StoredButBroadcastFailed")return {notify((result.state+": "+result.error+" "+result.backupId).toStdString()),winshun::effect::keep()};
            editorwin::onExternalChange(args.kind);
            return {notify(result.state=="Succeeded"?tr(zh,"已保存并核对","Saved and verified"):tr(zh,"已保存，广播失败","Saved; broadcast failed")),winshun::effect::hide()};
        }
        if((kind=="envRestore"||kind=="hostsRestore")&&a.action=="restore"){
            const auto args=buildEditorArgs(kind=="envRestore"?"env":"hosts","","",st);
            if(!editorwin::openEditorDeferred(args))return {notify("E_QML_INIT"),winshun::effect::keep()};
            return {notify(tr(zh,"请在“备份”页预览并恢复；保存后才写入目标。","Preview and restore in Backups, then Save to write the target.")),winshun::effect::hide()};
        }
#endif
        if (kind == "envEdit") {
            const std::string name = data["name"].toString();
            const std::string value = data["value"].toString();
            if (a.action == "cancel")
                return {winshun::effect::setQuery("env " + name), winshun::effect::keep()};
            if (a.action == "copy")
                return {winshun::effect::copy(value), notify(tr(zh, "已复制", "Copied")), winshun::effect::hide()};
            if (a.action == "save") {
                if (name.empty() || !envh::isValidEnvName(name))
                    return {notify(tr(zh, "变量名无效", "Invalid name")), winshun::effect::keep()};
                if (!envh::isValidEnvValue(value))
                    return {notify(tr(zh, "值无效", "Invalid value")), winshun::effect::keep()};
                envh::native::EnvStore store(testUserSubkey(), testSystemSubkey());
                envh::native::BackupStore backups(dataDir(), st.backupCount);
                backups.saveEnv(store.read(false).snap); // back up current state first
                const std::string type = envh::envTypeForValue(value);
                if (!store.writeVar(name, value, type)) {
                    winshun::debug("ra9fael.envhosts: writeVar failed for " + name);
                    return {notify(tr(zh, "写入失败（注册表拒绝）", "Write failed (registry denied)")), winshun::effect::keep()};
                }
                store.broadcastSettingsChange();
                editorwin::onExternalChange("env");
                return {notify(tr(zh, "已保存 ", "Saved ") + name), winshun::effect::setQuery("env " + name), winshun::effect::hide()};
            }
            return {notify(tr(zh, "未支持的动作", "Unsupported action")), winshun::effect::hide()};
        }

        // ---- env delete confirm ----
        if (kind == "envDeleteConfirm") {
            if (a.action == "cancel")
                return {winshun::effect::setQuery("env"), winshun::effect::keep()};
            const std::string target = data["target"].toString();
            const bool isSystem = target.compare(0, 7, "system/") == 0;
            const std::string name = target.substr(target.find('/') + 1);
            envh::native::EnvStore store(testUserSubkey(), testSystemSubkey());
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            if (isSystem) {
                // Elevated plugin (Plan A): write HKLM when the probe passes.
                std::string werr;
                if (!store.canWriteSystem(werr))
                    return {notify(tr(zh, "系统变量需要管理员权限：以管理员运行 Win顺 并在插件卡片点“确认”后重试（编辑器状态栏右下角“权限”有说明）",
                                        "System variables need administrator rights: run Win顺 as admin and confirm the plugin card, then retry (see the Perms button in the editor's status bar)")),
                            winshun::effect::keep()};
                backups.saveEnv(store.read(true).snap, true);
                if (!store.deleteVar(name, "system")) {
                    winshun::debug("ra9fael.envhosts: deleteVar failed for system/" + name);
                    return {notify(tr(zh, "删除失败", "Delete failed")), winshun::effect::keep()};
                }
            } else {
                backups.saveEnv(store.read(false).snap);
                if (!store.deleteVar(name)) {
                    winshun::debug("ra9fael.envhosts: deleteVar failed for " + name);
                    return {notify(tr(zh, "删除失败", "Delete failed")), winshun::effect::keep()};
                }
            }
            store.broadcastSettingsChange();
            editorwin::onExternalChange("env");
            return {notify(tr(zh, "已删除 ", "Deleted ") + name), winshun::effect::setQuery("env"), winshun::effect::hide()};
        }

        // ---- hosts ----
        if (kind == "hosts") {
            const std::string id = data["id"].toString();
            const std::string hostname = data["hostname"].toString();
            // Copy and open act on the queried row's text: no write access and
            // no re-read needed, so they work on a read-only hosts too.
            if (a.action == "copyIp")
                return {winshun::effect::copy(data["ip"].toString()), notify(tr(zh, "已复制 IP", "Copied IP")), winshun::effect::hide()};
            if (a.action == "copyHostname")
                return {winshun::effect::copy(hostname), notify(tr(zh, "已复制主机名", "Copied hostname")), winshun::effect::hide()};
            if (a.action == "copyLine")
                return {winshun::effect::copy(data["primary"].toString()), notify(tr(zh, "已复制整行", "Copied line")), winshun::effect::hide()};
            if (a.action == "openFile")
                return {winshun::effect::openPath(testHostsPath(st)), notify(tr(zh, "已打开 hosts 文件", "Opened hosts file")), winshun::effect::hide()};

            envh::native::HostsStore store(winshun::wide(testHostsPath(st)));
            const envh::native::HostsReadResult r = store.read();
            if (!r.ok)
                return {notify(r.errMsg), winshun::effect::keep()};
            envh::HostsParsed parsed = envh::parseHosts(r.content);
            int lineIndex = -1;

            if (a.action == "edit") {
                // Locate the record's current line; a record that moved is
                // still opened, just without a stale line number.
                if (envh::findHostsEntryById(parsed, id, lineIndex))
                    return openEditorTarget("hosts", hostname, std::to_string(lineIndex + 1), st, zh);
                const auto opened = openEditorTarget("hosts", hostname, "", st, zh);
                return {notify(tr(zh, "记录已变化，未定位；编辑器已打开", "Record changed; opened the editor without locating")),
                        opened};
            }

            // Write actions probe access only now, after copy/open/edit.
            std::string writableErr;
            if (!store.isWritable(writableErr))
                return {winshun::effect::replace(winshun::Items {errorRow("hosts:denied", "E_DENIED",
                                                                         tr(zh, "hosts 只读", "hosts is read-only"),
                                                                         tr(zh, "普通权限无法写入；可用“打开 hosts 文件”在外部编辑器中修改，或让 Win顺 以管理员运行并确认 elevated",
                                                                             "normal user cannot write; use \u201cOpen hosts file\u201d to edit it externally, or run Win顺 as admin with elevated"),
                                                                         writableErr, zh)}),
                        winshun::effect::keep()};

            if (!envh::findHostsEntryById(parsed, id, lineIndex)
                || parsed.lines[std::size_t(lineIndex)].text != data["primary"].toString())
                return {notify(tr(zh, "该行已被外部修改，列表已刷新", "The line changed externally; the list was refreshed")),
                        winshun::effect::setQuery("hosts " + hostname), winshun::effect::keep()};

            if (a.action == "enable" || a.action == "disable" || a.action == "toggle") {
                const auto out = envh::hostsToggle(parsed, lineIndex);
                if (!out.success)
                    return {notify(out.failure.message), winshun::effect::keep()};
                envh::native::BackupStore backups(dataDir(), st.backupCount);
                backups.saveHosts(r.content);
                std::string werr;
                if (!store.write(envh::serializeHosts(out.value), werr)) {
                    winshun::debug("ra9fael.envhosts: hosts write failed: " + werr);
                    return {notify(tr(zh, "写入失败：", "Write failed: ") + werr), winshun::effect::keep()};
                }
                editorwin::onExternalChange("hosts");
                // Keep the launcher list open: the query refreshes with the
                // same filter and shows the updated record.
                return {notify(tr(zh, "已切换注释状态", "Toggled comment")), winshun::effect::setQuery("hosts " + hostname), winshun::effect::keep()};
            }
            if (a.action == "delete") {
                // The confirm row carries the full identity (stable id with
                // occurrence, raw line) so the confirmed delete lands on this
                // exact line, not on the first text match.
                winshun::Json extra = winshun::Json(winshun::Json::Object {});
                extra.set("id", id);
                extra.set("raw", data["primary"].toString());
                extra.set("hostname", hostname);
                return {winshun::effect::replace(winshun::Items {confirmRow("hosts:del:" + envh::toLower(hostname), "hostsDeleteConfirm", data["primary"].toString(),
                                                                         tr(zh, "删除行 ", "delete line ") + data["primary"].toString(),
                                                                         tr(zh, "Enter 确认删除，Ctrl+Enter 取消", "Enter confirms, Ctrl+Enter cancels"), zh, extra)}),
                        winshun::effect::keep()};
            }
            return {notify(tr(zh, "未支持的动作", "Unsupported action")), winshun::effect::hide()};
        }

        // ---- hosts delete confirm ----
        if (kind == "hostsDeleteConfirm") {
            if (a.action == "cancel")
                return {winshun::effect::setQuery("hosts"), winshun::effect::keep()};
            envh::native::HostsStore store(winshun::wide(testHostsPath(st)));
            std::string writableErr;
            if (!store.isWritable(writableErr))
                return {notify(tr(zh, "hosts 只读，无法删除", "hosts read-only, cannot delete")), winshun::effect::keep()};
            const envh::native::HostsReadResult r = store.read();
            if (!r.ok)
                return {notify(r.errMsg), winshun::effect::keep()};
            envh::HostsParsed parsed = envh::parseHosts(r.content);
            // Resolve by the identity carried from the queried row (stable id
            // with occurrence) and verify the raw line before deleting.
            int lineIndex = -1;
            if (!envh::findHostsEntryById(parsed, data["id"].toString(), lineIndex)
                || parsed.lines[std::size_t(lineIndex)].text != data["raw"].toString())
                return {notify(tr(zh, "该行已被外部修改，列表已刷新", "The line changed externally; the list was refreshed")),
                        winshun::effect::setQuery("hosts " + data["hostname"].toString()), winshun::effect::keep()};
            const auto out = envh::hostsDelete(parsed, lineIndex);
            if (!out.success)
                return {notify(out.failure.message), winshun::effect::keep()};
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            backups.saveHosts(r.content);
            std::string werr;
            if (!store.write(envh::serializeHosts(out.value), werr))
                return {notify(tr(zh, "写入失败：", "Write failed: ") + werr), winshun::effect::keep()};
            editorwin::onExternalChange("hosts");
            return {notify(tr(zh, "已删除该行", "Deleted line")), winshun::effect::setQuery("hosts " + data["hostname"].toString()), winshun::effect::hide()};
        }

        // ---- hosts add confirm ----
        if (kind == "hostsAddConfirm") {
            if (a.action == "cancel")
                return {winshun::effect::setQuery("hosts"), winshun::effect::keep()};
            envh::native::HostsStore store(winshun::wide(testHostsPath(st)));
            std::string writableErr;
            if (!store.isWritable(writableErr))
                return {notify(tr(zh, "hosts 只读，无法添加", "hosts read-only, cannot add")), winshun::effect::keep()};
            const envh::native::HostsReadResult r = store.read();
            if (!r.ok)
                return {notify(r.errMsg), winshun::effect::keep()};
            const std::string target = data["target"].toString();
            // Split target back into ip/host/comment.
            std::string ip, host, comment;
            std::size_t sp1 = target.find(' ');
            ip = target.substr(0, sp1);
            std::string rest = target.substr(sp1 + 1);
            std::size_t sp2 = rest.find(' ');
            if (sp2 == std::string::npos) {
                host = rest;
            } else {
                host = rest.substr(0, sp2);
                std::string c = rest.substr(sp2 + 1);
                if (envh::startsWith(c, "# "))
                    c = c.substr(2);
                comment = c;
            }
            const auto add = envh::hostsAdd(envh::parseHosts(r.content), ip, host, comment);
            if (!add.success)
                return {notify(add.failure.message), winshun::effect::keep()};
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            backups.saveHosts(r.content);
            std::string werr;
            if (!store.write(envh::serializeHosts(add.value), werr))
                return {notify(tr(zh, "写入失败：", "Write failed: ") + werr), winshun::effect::keep()};
            editorwin::onExternalChange("hosts");
            return {notify(tr(zh, "已添加 ", "Added ") + target), winshun::effect::setQuery("hosts " + host), winshun::effect::hide()};
        }

        // ---- restore ----
        if (kind == "envRestore") {
            if (a.action != "restore")
                return {winshun::effect::setQuery("env"), winshun::effect::keep()};
            envh::native::EnvStore store(testUserSubkey(), testSystemSubkey());
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            backups.saveEnv(store.read(false).snap); // keep an undo point
            if (backups.restoreEnv(data["backupId"].toString(), store)) {
                editorwin::onExternalChange("env");
                return {notify(tr(zh, "已恢复备份", "Restored backup")), winshun::effect::setQuery("env"), winshun::effect::hide()};
            }
            return {notify(tr(zh, "恢复失败", "Restore failed")), winshun::effect::keep()};
        }
        if (kind == "hostsRestore") {
            if (a.action != "restore")
                return {winshun::effect::setQuery("hosts"), winshun::effect::keep()};
            envh::native::HostsStore store(winshun::wide(testHostsPath(st)));
            std::string writableErr;
            if (!store.isWritable(writableErr))
                return {notify(tr(zh, "hosts 只读，无法恢复", "hosts read-only, cannot restore")), winshun::effect::keep()};
            envh::native::BackupStore backups(dataDir(), st.backupCount);
            if (backups.restoreHosts(data["backupId"].toString(), store)) {
                editorwin::onExternalChange("hosts");
                return {notify(tr(zh, "已恢复备份", "Restored backup")), winshun::effect::setQuery("hosts"), winshun::effect::hide()};
            }
            return {notify(tr(zh, "恢复失败", "Restore failed")), winshun::effect::keep()};
        }

        return {notify(tr(zh, "未支持的动作", "Unsupported action")), winshun::effect::hide()};
    }

    // Always re-read from the OS (no host cache) so external edits show up.
    winshun::Json queryResult(const winshun::Query& q) override
    {
        return winshun::Json::Object {{"items", winshun::itemsJson(query(q))}, {"cache", 0}};
    }

    void settingsChanged(const winshun::Json& s) override
    {
        winshun::debug("ra9fael.envhosts: settingsChanged");
        const auto lang = envh::i18n::resolve(locale(), readSettings(s).uiLanguage);
        editorwin::onAppearanceChanged(lang, theme() == "dark", appearance().dump());
    }

    // WinShun sends `appearance` (with locale inside the look object) when its
    // language or theme changes. Push it to windows already open on their own
    // GUI threads; a new window picks up the same values from uiLang().
    void appearanceChanged() override
    {
        editorwin::onAppearanceChanged(uiLang(), theme() == "dark", appearance().dump());
    }

#ifdef ENVHOSTS_QML_EDITOR
    void shuttingDown() override { editorwin::closeAllEditors(1800); }
#endif


    void cancel(long long queryId) override { winshun::debug("ra9fael.envhosts: cancel " + std::to_string(queryId)); }
};

#else // !_WIN32

// On non-Windows the plugin cannot talk to the registry; the resident loop is
// still compiled so the project builds, but query() reports the platform limit.
struct EnvHosts : winshun::Plugin {
    std::string name() const override { return "ra9fael.envhosts"; }
    winshun::Items query(const winshun::Query&) override
    {
        return {errorRow("error.platform", "E_PLATFORM", "Windows only",
                         "this plugin edits the Windows registry and hosts file", "", false)};
    }
};

#endif

} // namespace

int main(int argc, char** argv)
{
    if(argc==2&&std::string(argv[1])=="--build-info") {
        std::cout << "{\"id\":\"ra9fael.envhosts\",\"version\":\"" << ENVHOSTS_VERSION
                  << "\",\"backend\":\"" << ENVHOSTS_BACKEND << "\"}\n";
        return 0;
    }
#ifdef ENVHOSTS_QML_EDITOR
    // Qt Quick requires the GUI thread, so the Qt loop owns the main thread and
    // the JSON-RPC loop moves to a worker. stdio stays owned by the protocol
    // thread; the GUI thread never writes to it.
    // The font engine WinShun itself uses with its hardware renderer: the UI
    // font (Alibaba PuHuiTi) has no hinting, and FreeType's vertical hinting
    // snaps its horizontal strokes to the grid where GDI smears them across two
    // pixel rows (docs/qml/MEMORY_FIX.md). An explicit platform selection
    // remains available for diagnostics.
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");
    QGuiApplication app(argc, argv);
    setPluginWindowIcon();
    app.setQuitOnLastWindowClosed(false);
    editorwin::initialize();
    std::thread protocol([] {
        EnvHosts().serve();
        editorwin::shutdownWhenReady();
    });
    const int rc = app.exec();
    protocol.join();
    return rc;
#else
    return EnvHosts().serve();
#endif
}
