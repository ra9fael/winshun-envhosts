// qt_editor.cpp - Qt Quick editor front-end implementation (migration prototype).
//
// Everything below runs on the GUI thread except the four protocol-facing entry
// points, which marshal to it. Read-only slice: no writes, no drafts.
//
// Code and comments are English.
#ifdef ENVHOSTS_QML_EDITOR

#include "qt_editor.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThread>
#include <QVariant>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

#include "editor_model.h"

namespace editorwin {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString joinHosts(const std::vector<std::string>& hs)
{
    QString out;
    for (const auto& h : hs) {
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += qs(h);
    }
    return out;
}

bool dominantCrlf(const envh::HostsParsed& p)
{
    int crlf = 0, lf = 0;
    for (const auto& l : p.lines)
        (l.crlf ? crlf : lf)++;
    return crlf >= lf;
}

struct WindowState {
    QQuickWindow* window = nullptr;
    EditorBridge* bridge = nullptr;
};

class EditorController : public QObject {
public:
    QQmlEngine engine;
    std::map<std::string, WindowState> windows; // GUI thread only

    std::mutex lock; // guards open state for the protocol thread
    std::condition_variable cv;
    std::map<std::string, bool> open;

    void markOpen(const std::string& kind, bool value)
    {
        {
            std::lock_guard<std::mutex> lk(lock);
            open[kind] = value;
        }
        cv.notify_all();
    }

    bool createWindow(const OpenArgs& a, std::string& error)
    {
        auto it = windows.find(a.kind);
        if (it != windows.end() && it->second.window) {
            it->second.bridge->applyFocus(a);
            it->second.window->show();
            it->second.window->raise();
            it->second.window->requestActivate();
            return true;
        }

        auto* bridge = new EditorBridge(a);
        QString err;
        if (!bridge->load(err)) {
            // Keep the window open: it shows the load error in place of the table.
        }

        QQmlComponent comp(&engine,
                           QUrl(QStringLiteral("qrc:/qt/qml/EnvHosts/Editor/qt/qml/Main.qml")));
        if (comp.isError()) {
            error = "qml: " + comp.errorString().toStdString();
            delete bridge;
            return false;
        }
        QObject* obj = comp.createWithInitialProperties(
            {{QStringLiteral("bridge"), QVariant::fromValue(static_cast<QObject*>(bridge))}});
        if (!obj) {
            error = "qml: " + comp.errorString().toStdString();
            delete bridge;
            return false;
        }
        QQuickWindow* win = qobject_cast<QQuickWindow*>(obj);
        if (!win) {
            error = "qml root is not a Window";
            delete obj;
            delete bridge;
            return false;
        }
        bridge->setParent(win);
        windows[a.kind] = WindowState {win, bridge};
        markOpen(a.kind, true);

        const std::string kind = a.kind;
        QObject::connect(win, &QObject::destroyed, this, [this, kind] {
            windows.erase(kind);
            markOpen(kind, false);
        });

        win->show();
        win->raise();
        win->requestActivate();
        return true;
    }

    void closeWindow(const std::string& kind)
    {
        auto it = windows.find(kind);
        if (it != windows.end() && it->second.window)
            it->second.window->close();
    }
};

EditorController* g_ctl = nullptr;

// Runs `fn` on the GUI thread; returns false when the event loop is gone.
template <typename F>
bool onGuiThread(F&& fn)
{
    if (!qApp)
        return false;
    if (QThread::currentThread() == qApp->thread())
        return fn();
    bool ok = false;
    const bool posted = QMetaObject::invokeMethod(
        qApp, [&fn, &ok] { ok = fn(); }, Qt::BlockingQueuedConnection);
    return posted && ok;
}

} // namespace

// ---- EditorRows ---------------------------------------------------------------

QVariant EditorRows::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_rows.size())
        return {};
    const Row& r = m_rows.at(idx.row());
    switch (role) {
    case CellsRole:
        return r.cells;
    case DataRole:
        return r.data;
    case KindRole:
        return r.kind;
    default:
        return {};
    }
}

QHash<int, QByteArray> EditorRows::roleNames() const
{
    return {{CellsRole, "cells"}, {DataRole, "data"}, {KindRole, "kind"}};
}

void EditorRows::resetRows(QVector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

// ---- EditorBridge -------------------------------------------------------------

EditorBridge::EditorBridge(OpenArgs args, QObject* parent)
    : QObject(parent), m_args(std::move(args)), m_kind(m_args.kind)
{
    m_filterText = qs(m_args.filter);
}

QString EditorBridge::title() const
{
    if (m_kind == "env")
        return m_args.zh ? "环境变量 · QML 预览" : "Environment variables · QML preview";
    return m_args.zh ? "hosts · QML 预览" : "hosts · QML preview";
}

QStringList EditorBridge::columns() const
{
    if (m_kind == "env")
        return m_args.zh ? QStringList {"名称", "值", "范围 / 类型"}
                         : QStringList {"Name", "Value", "Scope / type"};
    return m_args.zh ? QStringList {"状态", "行", "IP", "主机名", "备注"}
                     : QStringList {"State", "Line", "IP", "Host names", "Comment"};
}

bool EditorBridge::writable() const
{
    return m_kind == "env" ? m_args.systemWritable : m_args.writable;
}

QString EditorBridge::detailTitle() const
{
    if (m_visible.empty() || m_sel < 0 || m_sel >= static_cast<int>(m_visible.size()))
        return {};
    const int idx = m_visible[static_cast<size_t>(m_sel)];
    return m_rows.data(m_rows.index(idx, 0), EditorRows::DataRole)
        .toMap()
        .value("title")
        .toString();
}

QVariantList EditorBridge::detailFields() const
{
    if (m_visible.empty() || m_sel < 0 || m_sel >= static_cast<int>(m_visible.size()))
        return {};
    const QVariantMap m = m_rows.data(m_rows.index(m_visible[static_cast<size_t>(m_sel)], 0),
                                     EditorRows::DataRole)
                              .toMap();
    return m.value("fields").toList();
}

QString EditorBridge::statusLine() const
{
    const int n = static_cast<int>(m_visible.size());
    if (m_kind == "env") {
        const QString perm = m_args.systemWritable
            ? (m_args.zh ? "管理员：用户 / 系统均可写" : "admin: user and system writable")
            : (m_args.zh ? "用户可写 / 系统只读" : "user writable / system read-only");
        return (m_args.zh ? QString("共 %1 行 · ") : QString("%1 rows · ")).arg(n) + perm;
    }
    const QString perm = m_args.writable ? (m_args.zh ? "可写" : "writable")
                                         : (m_args.zh ? "只读（需管理员）" : "read-only (needs admin)");
    return (m_args.zh ? QString("共 %1 行 · ") : QString("%1 rows · ")).arg(n) + perm;
}

bool EditorBridge::load(QString& error)
{
    std::lock_guard<std::mutex> lk(envh::native::g_writeLock);
    if (m_kind == "env") {
        envh::native::EnvStore store(m_args.userSubkey, m_args.sysSubkey);
        const envh::native::EnvReadResult r = store.read(m_args.showSystem);
        if (!r.ok) {
            error = qs(r.errMsg);
            setError(error);
            return false;
        }
        m_env = envh::edit::EnvModel::fromSnapshot(r.snap);
    } else {
        envh::native::HostsStore store(m_args.hostsPath);
        const envh::native::HostsReadResult r = store.read();
        if (!r.ok) {
            error = qs(r.errMsg);
            setError(error);
            return false;
        }
        const envh::HostsParsed p = envh::parseHosts(r.content);
        m_hosts = envh::edit::HostsModel::fromParsed(p, dominantCrlf(p));
    }
    m_hasModel = true;
    setError({});
    rebuild();
    return true;
}

void EditorBridge::reload()
{
    QString err;
    load(err);
}

void EditorBridge::applyFocus(const OpenArgs& a)
{
    if (!a.filter.empty() && a.filter != m_args.filter) {
        m_args.filter = a.filter;
        m_filterText = qs(a.filter);
        rebuild();
        emit filterChanged();
    }
    m_args.select = a.select;
    if (!a.select.empty()) {
        // Prototype: only the filter is honoured; selection lookup lands with the
        // editing slice.
        m_sel = 0;
        emit currentRowChanged();
    }
}

void EditorBridge::setFilter(const QString& text)
{
    if (text == m_filterText)
        return;
    m_filterText = text;
    rebuild();
    emit filterChanged();
}

void EditorBridge::selectRow(int row)
{
    if (row < 0 || row >= static_cast<int>(m_visible.size()) || row == m_sel)
        return;
    m_sel = row;
    emit currentRowChanged();
}

void EditorBridge::setError(const QString& text)
{
    if (text == m_error)
        return;
    m_error = text;
    emit errorChanged();
}

void EditorBridge::rebuild()
{
    if (!m_hasModel)
        return;

    QVector<EditorRows::Row> rows;
    m_visible.clear();
    const QString needle = m_filterText.trimmed().toLower();
    const auto matches = [&needle](const QString& hay) {
        return needle.isEmpty() || hay.toLower().contains(needle);
    };

    if (m_kind == "env") {
        for (size_t i = 0; i < m_env.rows.size(); ++i) {
            const envh::edit::EnvVarRow& v = m_env.rows[i];
            const QString name = qs(v.name);
            const QString value = qs(v.value);
            if (!matches(name) && !matches(value))
                continue;
            m_visible.push_back(static_cast<int>(i));
            EditorRows::Row row;
            row.cells = {name, value,
                         (v.scope == "user" ? (m_args.zh ? "用户 · " : "user · ")
                                            : (m_args.zh ? "系统 · " : "system · "))
                             + qs(v.type)};
            row.data = {{"title", name},
                        {"fields", QVariantList {
                                       QVariantMap {{"label", m_args.zh ? "名称" : "Name"}, {"value", name}},
                                       QVariantMap {{"label", m_args.zh ? "值" : "Value"}, {"value", value}},
                                       QVariantMap {{"label", m_args.zh ? "类型" : "Type"}, {"value", qs(v.type)}},
                                       QVariantMap {{"label", m_args.zh ? "范围" : "Scope"},
                                                    {"value", v.scope == "user"
                                                                  ? (m_args.zh ? "用户" : "user")
                                                                  : (m_args.zh ? "系统" : "system")}}}}};
            if (v.deleted)
                row.kind = 3;
            rows.append(row);
        }
    } else {
        const std::vector<bool> dup = m_hosts.duplicateFlags();
        for (size_t i = 0; i < m_hosts.rows.size(); ++i) {
            const envh::edit::HostsRow& h = m_hosts.rows[i];
            const QString hostnames = joinHosts(h.hostnames);
            if (h.isSection) {
                const QString text = qs(h.sectionText);
                if (!matches(text))
                    continue;
                m_visible.push_back(static_cast<int>(i));
                EditorRows::Row row;
                row.cells = {text};
                row.data = {{"title", text}, {"fields", QVariantList {}}};
                row.kind = 1;
                rows.append(row);
                continue;
            }
            const QString ip = qs(h.ip);
            if (!matches(ip) && !matches(hostnames) && !matches(qs(h.comment)))
                continue;
            m_visible.push_back(static_cast<int>(i));
            EditorRows::Row row;
            row.cells = {h.commented ? (m_args.zh ? "停用" : "off") : (m_args.zh ? "启用" : "on"),
                         QString::number(h.origin + 1), ip + (i < dup.size() && dup[i] ? " !" : ""),
                         hostnames, qs(h.comment)};
            row.data = {{"title", ip.isEmpty() ? hostnames : (ip + "  " + hostnames)},
                        {"fields", QVariantList {
                                       QVariantMap {{"label", m_args.zh ? "IP" : "IP"}, {"value", ip}},
                                       QVariantMap {{"label", m_args.zh ? "主机名" : "Host names"}, {"value", hostnames}},
                                       QVariantMap {{"label", m_args.zh ? "备注" : "Comment"}, {"value", qs(h.comment)}},
                                       QVariantMap {{"label", m_args.zh ? "状态" : "State"},
                                                    {"value", h.commented ? (m_args.zh ? "已注释" : "commented")
                                                                          : (m_args.zh ? "生效" : "active")}}}}};
            if (h.deleted)
                row.kind = 3;
            rows.append(row);
        }
    }

    m_rows.resetRows(std::move(rows));
    m_sel = m_visible.empty() ? 0 : std::min(m_sel, static_cast<int>(m_visible.size()) - 1);
    emit currentRowChanged();
    emit statusChanged();
}

// ---- protocol-facing API ------------------------------------------------------

void initialize()
{
    if (g_ctl)
        return;
    // Basic is the only style whose QML module ships with this deployment; the
    // native Windows style would need its own plugin DSOs next to the exe.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    g_ctl = new EditorController;
    g_ctl->engine.setParent(QCoreApplication::instance());
}

bool openEditor(const OpenArgs& a, std::string& error)
{
    if (!g_ctl) {
        error = "Qt editor is not initialised";
        return false;
    }
    EditorController* ctl = g_ctl;
    std::string err;
    const bool ok = onGuiThread([&] { return ctl->createWindow(a, err); });
    error = err;
    if (ok)
        ctl->markOpen(a.kind, true);
    return ok;
}

bool isEditorOpen(const std::string& kind)
{
    if (!g_ctl)
        return false;
    std::lock_guard<std::mutex> lk(g_ctl->lock);
    const auto it = g_ctl->open.find(kind);
    return it != g_ctl->open.end() && it->second;
}

void onExternalChange(const std::string& kind)
{
    if (!qApp || !g_ctl)
        return;
    EditorController* ctl = g_ctl;
    QMetaObject::invokeMethod(
        qApp,
        [ctl, kind] {
            auto it = ctl->windows.find(kind);
            if (it != ctl->windows.end() && it->second.bridge)
                it->second.bridge->reload();
        },
        Qt::QueuedConnection);
}

void closeAllEditors(unsigned timeoutMs)
{
    if (!g_ctl)
        return;
    EditorController* ctl = g_ctl;
    onGuiThread([&] {
        std::vector<std::string> kinds;
        for (const auto& kv : ctl->windows)
            kinds.push_back(kv.first);
        for (const auto& k : kinds)
            ctl->closeWindow(k);
        return true;
    });
    // The windows mark themselves closed through the destroyed signal; wait so
    // the protocol thread does not race the GUI thread on shutdown.
    std::unique_lock<std::mutex> lk(ctl->lock);
    ctl->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
        for (const auto& kv : ctl->open)
            if (kv.second)
                return false;
        return true;
    });
}

} // namespace editorwin

#endif // ENVHOSTS_QML_EDITOR
