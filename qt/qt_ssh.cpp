// qt_ssh.cpp - implementation of the SSH window's Qt bridge.
//
// Every write goes through ssh_model.h and native.h; this file owns only the
// translation into QML shapes, the dialogs' data and the clipboard.
//
// Code and comments are English.
#include "qt_ssh.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QStringList>

#include <algorithm>
#include <array>
#include <string>

#include "native.h"

namespace editorwin {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString fieldLabel(const std::string& key)
{
    if (key == "alias")
        return "Host";
    if (key == "hostname")
        return "HostName";
    if (key == "user")
        return "User";
    if (key == "port")
        return "Port";
    if (key == "identity")
        return "IdentityFile";
    if (key == "proxyjump")
        return "ProxyJump";
    return qs(key);
}

QStringList displayLines(const QString& text)
{
    QStringList out;
    out = text.split('\n');
    for (QString& line : out)
        if (line.endsWith('\r'))
            line.chop(1);
    // A trailing newline produces one empty element, which is not a line.
    if (!out.isEmpty() && out.back().isEmpty() && text.endsWith('\n'))
        out.pop_back();
    return out;
}

QVariantList diffLists(const QStringList& a, const QStringList& b)
{
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    std::vector<std::vector<int>> dp(static_cast<size_t>(n + 1), std::vector<int>(static_cast<size_t>(m + 1)));
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            dp[static_cast<size_t>(i)][static_cast<size_t>(j)] =
                a[i] == b[j] ? dp[static_cast<size_t>(i + 1)][static_cast<size_t>(j + 1)] + 1
                             : std::max(dp[static_cast<size_t>(i + 1)][static_cast<size_t>(j)],
                                        dp[static_cast<size_t>(i)][static_cast<size_t>(j + 1)]);

    QVariantList out;
    int i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.append(QVariantMap {{"t", " "}, {"s", a[i]}});
            ++i;
            ++j;
        } else if (dp[static_cast<size_t>(i + 1)][static_cast<size_t>(j)]
                   >= dp[static_cast<size_t>(i)][static_cast<size_t>(j + 1)]) {
            out.append(QVariantMap {{"t", "-"}, {"s", a[i]}});
            ++i;
        } else {
            out.append(QVariantMap {{"t", "+"}, {"s", b[j]}});
            ++j;
        }
    }
    while (i < n)
        out.append(QVariantMap {{"t", "-"}, {"s", a[i++]}});
    while (j < m)
        out.append(QVariantMap {{"t", "+"}, {"s", b[j++]}});
    return out;
}

QString capped(const std::string& text)
{
    QString s = qs(text).trimmed();
    if (s.size() > 500)
        s = s.left(500) + QStringLiteral("...");
    return s;
}

QVariantMap actionResult(const QString& code, const QString& detail = QString(), const QString& value = QString())
{
    QVariantMap result;
    result[QStringLiteral("code")] = code;
    if (!detail.isEmpty())
        result[QStringLiteral("detail")] = detail;
    if (!value.isEmpty())
        result[QStringLiteral("value")] = value;
    return result;
}

} // namespace

// ---- models -------------------------------------------------------------------

QVariant SshConnModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_rows.size())
        return {};
    const Row& r = m_rows.at(idx.row());
    switch (role) {
    case NameRole:
        return r.name;
    case HostRole:
        return r.host;
    case UserRole:
        return r.user;
    case PortRole:
        return r.port;
    case KeyRole:
        return r.key;
    case MarkRole:
        return r.mark;
    case LockedRole:
        return r.locked;
    case DeletedRole:
        return r.deleted;
    case NewRole:
        return r.isNew;
    case ConnRole:
        return r.conn;
    default:
        return {};
    }
}

QHash<int, QByteArray> SshConnModel::roleNames() const
{
    return {{NameRole, "name"},   {HostRole, "host"},       {UserRole, "user"},
            {PortRole, "port"},   {KeyRole, "key"},         {MarkRole, "mark"},
            {LockedRole, "locked"}, {DeletedRole, "deleted"}, {NewRole, "isNew"},
            {ConnRole, "conn"}};
}

void SshConnModel::resetRows(QVector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

QVariant SshKeyModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_rows.size())
        return {};
    const Row& r = m_rows.at(idx.row());
    switch (role) {
    case NameRole:
        return r.name;
    case TypeRole:
        return r.type;
    case CommentRole:
        return r.comment;
    case FingerprintRole:
        return r.fingerprint;
    case StateRole:
        return r.state;
    case UsedByRole:
        return r.usedBy;
    case KeyRole:
        return r.key;
    default:
        return {};
    }
}

QHash<int, QByteArray> SshKeyModel::roleNames() const
{
    return {{NameRole, "name"},     {TypeRole, "type"},             {CommentRole, "comment"},
            {FingerprintRole, "fingerprint"}, {StateRole, "keyState"},  {UsedByRole, "usedBy"},
            {KeyRole, "key"}};
}

void SshKeyModel::resetRows(QVector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

// ---- bridge -------------------------------------------------------------------

SshBridge::SshBridge(OpenArgs args, QObject* parent) : QObject(parent), m_args(std::move(args))
{
    if (m_args.sshDir.empty())
        m_args.sshDir = envh::native::defaultSshDir();
    m_dirText = QString::fromStdWString(m_args.sshDir);
    m_configText = QDir::toNativeSeparators(QDir(m_dirText).filePath(QStringLiteral("config")));
    m_filter = qs(m_args.filter);
    parseAppearance(m_args.appearanceJson);
    refreshKnownHosts();
    m_keygenAvailable = !envh::native::findSshKeygen().empty();
    refreshAgent();
}

QString SshBridge::title() const
{
    return m_args.zh ? QStringLiteral("SSH 管理器 — Win顺") : QStringLiteral("SSH manager — WinShun");
}

QStringList SshBridge::privateKeyNames() const
{
    QStringList names;
    for (const auto& key : m_keys)
        if (key.state != envh::ssh::KeyState::PublicOnly)
            names.append(QString::fromStdString(key.name));
    return names;
}

QString SshBridge::backupDir() const
{
    return QDir::toNativeSeparators(QString::fromStdString(m_args.dataDir + "/backups/ssh"));
}

bool SshBridge::hasLocked() const
{
    for (const envh::ssh::Block& b : m_cfg.blocks())
        if (envh::ssh::isLocked(b))
            return true;
    return false;
}

bool SshBridge::emptyState() const
{
    return m_loaded && !m_hasConfig && m_cfg.blocks().empty() && !m_cfg.dirty();
}

bool SshBridge::load(QString& error)
{
    m_loaded = false;
    if (m_args.sshDir.empty()) {
        m_loadError = QStringLiteral("cannot resolve %USERPROFILE%\\.ssh");
        error = m_loadError;
        emit metaChanged();
        return false;
    }
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    const envh::native::SshReadResult r = store.read();
    if (!r.ok) {
        m_loadError = qs(r.errMsg);
        // The keys tab is independent of the config; a UTF-16 or unreadable
        // config must not hide the key list.
        refreshKeys();
        refreshKnownHosts();
        rebuild();
        emit metaChanged();
        emit changed();
        error = m_loadError;
        return false;
    }
    m_hasConfig = !r.missing;
    m_cfg = r.missing ? envh::ssh::Config::empty() : envh::ssh::parseConfig(r.content);
    m_loadError.clear();
    refreshKeys();
    refreshKnownHosts();
    refreshAgent();
    m_loaded = true;
    rebuild();
    applyFocus(m_args);
    emit metaChanged();
    emit changed();
    return true;
}

void SshBridge::reload()
{
    if (!m_loaded) {
        QString err;
        load(err);
    } else {
        reread(true);
    }
}

void SshBridge::reread(bool keepSelection)
{
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    const envh::native::SshReadResult r = store.read();
    if (!r.ok) {
        m_loadError = qs(r.errMsg);
        emit metaChanged();
        emit toast(QStringLiteral("load"), m_loadError);
        return;
    }
    m_loadError.clear();
    m_hasConfig = !r.missing;
    m_cfg = r.missing ? envh::ssh::Config::empty() : envh::ssh::parseConfig(r.content);
    refreshKeys();
    refreshKnownHosts();
    if (!keepSelection) {
        m_connSel = 0;
        m_keySel = 0;
    }
    m_conflictVisible = false;
    m_conflictDiff.clear();
    rebuild();
    emit metaChanged();
    emit changed();
    emit conflictChanged();
}

void SshBridge::parseAppearance(const std::string& appearanceJson)
{
    m_appearance.clear();
    if (!appearanceJson.empty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(appearanceJson));
        if (doc.isObject())
            m_appearance = doc.object().toVariantMap();
    }
}

void SshBridge::setAppearance(envh::i18n::Lang lang, bool dark, const std::string& appearanceJson)
{
    m_args.setLang(lang);
    m_args.dark = dark;
    parseAppearance(appearanceJson);
    emit metaChanged();
    emit detailChanged();
}

void SshBridge::applyFocus(const OpenArgs& a)
{
    if (!a.filter.empty() && a.filter != m_args.filter) {
        m_args.filter = a.filter;
        m_filter = qs(a.filter);
        emit filterChanged();
        rebuild();
    }
    if (!a.select.empty()) {
        const QString want = qs(a.select);
        const bool keys = want.startsWith(QStringLiteral("key:"));
        const QString name = keys ? want.mid(4) : want;
        if (keys) {
            for (std::size_t i = 0; i < m_visibleKeys.size(); ++i) {
                const QString n = qs(m_keys[static_cast<size_t>(m_visibleKeys[i])].name);
                if (n == name) {
                    m_keySel = static_cast<int>(i);
                    emit selectionChanged();
                    emit detailChanged();
                    break;
                }
            }
        } else {
            for (std::size_t i = 0; i < m_visibleConn.size(); ++i) {
                const envh::ssh::Block* b = m_cfg.connection(m_visibleConn[i]);
                if (!b)
                    continue;
                if (envh::ssh::displayName(*b) == name.toStdString()) {
                    m_connSel = static_cast<int>(i);
                    emit selectionChanged();
                    emit detailChanged();
                    break;
                }
            }
        }
    }
}

void SshBridge::refreshKeys()
{
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    m_keys = store.keys();
}

void SshBridge::refreshKnownHosts()
{
    const std::vector<int> selectedLines = [&] {
        std::vector<int> lines;
        for (const QVariant& row : m_selectedKnownHosts) {
            const int visible = row.toInt();
            if (visible >= 0 && visible < static_cast<int>(m_visibleKnownHosts.size()))
                lines.push_back(m_knownHosts[static_cast<std::size_t>(m_visibleKnownHosts[static_cast<std::size_t>(visible)])].line);
        }
        return lines;
    }();
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    const auto read = store.readKnownHosts();
    m_knownHostsError = read.ok ? QString() : qs(read.errMsg);
    m_knownHostsText = read.content;
    m_knownHosts = read.ok ? envh::ssh::parseKnownHosts(read.content) : std::vector<envh::ssh::KnownHost>();
    m_visibleKnownHosts.clear();
    const std::string needle = m_knownFilter.trimmed().toStdString();
    std::vector<std::pair<int, int>> knownMatches;
    for (int i = 0; i < static_cast<int>(m_knownHosts.size()); ++i) {
        const auto& host = m_knownHosts[static_cast<std::size_t>(i)];
        const int score = envh::ssh::fuzzyScore(host.hosts + " " + host.port + " " + host.type + " " + host.fingerprint, needle);
        if (score > -1000000)
            knownMatches.emplace_back(i, score);
    }
    std::stable_sort(knownMatches.begin(), knownMatches.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& match : knownMatches)
        m_visibleKnownHosts.push_back(match.first);
    QVariantList selected;
    for (int visiblePos = 0; visiblePos < static_cast<int>(m_visibleKnownHosts.size()); ++visiblePos) {
        const int visible = m_visibleKnownHosts[static_cast<std::size_t>(visiblePos)];
        if (std::find(selectedLines.begin(), selectedLines.end(), m_knownHosts[static_cast<std::size_t>(visible)].line) != selectedLines.end())
            selected.append(visiblePos);
    }
    m_selectedKnownHosts = selected;
    emit knownHostsChanged();
}

QVariantMap SshBridge::refreshAgent()
{
    m_agentKeys.clear();
    m_agentError.clear();
    const std::wstring tool = envh::native::findSshAdd();
    if (tool.empty()) {
        m_agentError = m_args.zh ? QStringLiteral("未找到 ssh-add.exe") : QStringLiteral("ssh-add.exe was not found");
        emit agentChanged();
        return actionResult(QStringLiteral("tool"), m_agentError);
    }
    const envh::native::ProcResult proc = envh::native::runProcessCapture(tool, {L"-l"});
    const QString output = QString::fromUtf8(proc.out.data(), static_cast<int>(proc.out.size()));
    const QStringList lines = output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    bool emptyAgent = false;
    const QRegularExpression linePattern(QStringLiteral("^\\s*(\\d+)\\s+(SHA256:[^\\s]+)\\s*(.*?)\\s+\\(([^()]*)\\)\\s*$"));
    for (const QString& line : lines) {
        if (line.contains(QStringLiteral("agent has no identities"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("no identities"), Qt::CaseInsensitive)) {
            emptyAgent = true;
            continue;
        }
        const QRegularExpressionMatch match = linePattern.match(line);
        if (!match.hasMatch())
            continue;
        QVariantMap row;
        row[QStringLiteral("bits")] = match.captured(1).toInt();
        row[QStringLiteral("fingerprint")] = match.captured(2);
        row[QStringLiteral("comment")] = match.captured(3).trimmed();
        row[QStringLiteral("type")] = match.captured(4).trimmed();
        row[QStringLiteral("source")] = QString();
        for (const auto& key : m_keys) {
            if (key.pub.ok && QString::fromStdString(key.pub.fingerprint) == match.captured(2)
                && key.state != envh::ssh::KeyState::PublicOnly) {
                row[QStringLiteral("source")] = QDir::toNativeSeparators(QDir(m_dirText).filePath(QString::fromStdString(key.name)));
                break;
            }
        }
        m_agentKeys.append(row);
    }
    if (!proc.launched)
        m_agentError = qs(proc.err);
    else if (!proc.ok && !emptyAgent && m_agentKeys.isEmpty())
        m_agentError = output.trimmed().isEmpty() ? QStringLiteral("ssh-add -l failed") : output.trimmed();
    emit agentChanged();
    if (!m_agentError.isEmpty())
        return actionResult(QStringLiteral("agent"), m_agentError);
    return actionResult(QString(), QString(), QString::number(m_agentKeys.size()));
}

QVariantMap SshBridge::addAgentKey(const QString& name)
{
    const std::wstring tool = envh::native::findSshAdd();
    if (tool.empty())
        return actionResult(QStringLiteral("tool"), m_args.zh ? QStringLiteral("未找到 ssh-add.exe") : QStringLiteral("ssh-add.exe was not found"));
    QString file;
    for (const auto& key : m_keys) {
        if (QString::fromStdString(key.name) == name && key.state != envh::ssh::KeyState::PublicOnly) {
            file = QDir::toNativeSeparators(QDir(m_dirText).filePath(name));
            break;
        }
    }
    if (file.isEmpty())
        return actionResult(QStringLiteral("key"), m_args.zh ? QStringLiteral("未找到对应私钥文件") : QStringLiteral("The private key file was not found"));
    const envh::native::ProcResult proc = envh::native::runInteractiveProcess(tool, {L"--", file.toStdWString()});
    if (!proc.launched || !proc.ok)
        return actionResult(QStringLiteral("agent"), proc.err.empty() ? QStringLiteral("ssh-add failed") : qs(proc.err));
    return refreshAgent();
}

QVariantMap SshBridge::removeAgentKeys(const QVariantList& fingerprints)
{
    const std::wstring tool = envh::native::findSshAdd();
    if (tool.empty())
        return actionResult(QStringLiteral("tool"), m_args.zh ? QStringLiteral("未找到 ssh-add.exe") : QStringLiteral("ssh-add.exe was not found"));
    int removed = 0;
    for (const QVariant& value : fingerprints) {
        const QString fingerprint = value.toString();
        QString source;
        for (const QVariant& item : m_agentKeys) {
            const QVariantMap row = item.toMap();
            if (row.value(QStringLiteral("fingerprint")).toString() == fingerprint) {
                source = row.value(QStringLiteral("source")).toString();
                break;
            }
        }
        if (source.isEmpty())
            return actionResult(QStringLiteral("source"), m_args.zh ? QStringLiteral("选中的 agent 密钥没有对应的本地公钥/私钥文件") : QStringLiteral("The selected agent key has no matching local key file"));
        const envh::native::ProcResult proc = envh::native::runProcessCapture(tool, {L"-d", source.toStdWString()});
        if (!proc.ok)
            return actionResult(QStringLiteral("agent"), proc.out.empty() ? QStringLiteral("ssh-add -d failed") : QString::fromUtf8(proc.out.data(), static_cast<int>(proc.out.size())).trimmed());
        ++removed;
    }
    const QVariantMap result = refreshAgent();
    if (result.value(QStringLiteral("code")).toString().isEmpty())
        return actionResult(QString(), QString(), QString::number(removed));
    return result;
}

QVariantMap SshBridge::clearAgent()
{
    const std::wstring tool = envh::native::findSshAdd();
    if (tool.empty())
        return actionResult(QStringLiteral("tool"), m_args.zh ? QStringLiteral("未找到 ssh-add.exe") : QStringLiteral("ssh-add.exe was not found"));
    const envh::native::ProcResult proc = envh::native::runProcessCapture(tool, {L"-D"});
    if (!proc.ok)
        return actionResult(QStringLiteral("agent"), proc.out.empty() ? QStringLiteral("ssh-add -D failed") : QString::fromUtf8(proc.out.data(), static_cast<int>(proc.out.size())).trimmed());
    return refreshAgent();
}

QVariantList SshBridge::knownHosts() const
{
    QVariantList rows;
    for (int visible : m_visibleKnownHosts) {
        const auto& entry = m_knownHosts[static_cast<std::size_t>(visible)];
        QVariantMap row;
        row["line"] = entry.line;
        row["hosts"] = qs(entry.hosts);
        row["port"] = qs(entry.port.empty() ? "22" : entry.port);
        row["portDisplay"] = entry.port.empty() ? QString() : qs(entry.port);
        row["type"] = qs(entry.type);
        row["fingerprint"] = qs(entry.fingerprint);
        row["raw"] = qs(entry.raw);
        rows.append(row);
    }
    return rows;
}

int SshBridge::deleteConnections(const QVariantList& rows)
{
    std::vector<int> connections;
    for (const QVariant& row : rows) {
        const int visible = row.toInt();
        if (visible < 0 || visible >= static_cast<int>(m_visibleConn.size()))
            continue;
        const int ci = m_visibleConn[static_cast<size_t>(visible)];
        const envh::ssh::Block* block = m_cfg.connection(ci);
        if (block && envh::ssh::isEditable(*block) && !block->deleted)
            connections.push_back(ci);
    }
    if (connections.empty())
        return 0;
    const int deleted = m_cfg.deleteMany(std::move(connections));
    if (deleted == 0)
        return 0;
    rebuild();
    emit detailChanged();
    emit changed();
    return deleted;
}

QStringList SshBridge::publicKeyFiles(const QVariantList& rows) const
{
    QStringList files;
    std::vector<std::string> names;
    for (const QVariant& row : rows) {
        const int visible = row.toInt();
        if (visible < 0 || visible >= static_cast<int>(m_visibleKeys.size()))
            continue;
        const auto& key = m_keys[static_cast<size_t>(m_visibleKeys[static_cast<size_t>(visible)])];
        if (key.state == envh::ssh::KeyState::PrivateOnly)
            continue;
        const std::string file = key.state == envh::ssh::KeyState::PublicOnly ? key.name : key.name + ".pub";
        if (std::find(names.begin(), names.end(), file) == names.end()) {
            names.push_back(file);
            files.append(qs(file));
        }
    }
    return files;
}

QVariantMap SshBridge::deletePublicKeys(const QVariantList& rows)
{
    QVariantMap result;
    QStringList names;
    std::vector<std::string> files;
    std::vector<std::string> selectedFiles;
    for (const QVariant& row : rows) {
        const int visible = row.toInt();
        if (visible < 0 || visible >= static_cast<int>(m_visibleKeys.size()))
            continue;
        const auto& key = m_keys[static_cast<size_t>(m_visibleKeys[static_cast<size_t>(visible)])];
        if (key.state == envh::ssh::KeyState::PrivateOnly)
            continue;
        const std::string file = key.state == envh::ssh::KeyState::PublicOnly ? key.name : key.name + ".pub";
        if (std::find(selectedFiles.begin(), selectedFiles.end(), file) == selectedFiles.end()) {
            selectedFiles.push_back(file);
            files.push_back(file);
            names.append(qs(file));
        }
    }
    if (files.empty()) {
        result["code"] = QStringLiteral("noPublicKeys");
        return result;
    }
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    std::string error;
    if (!store.deletePublicKeys(files, error)) {
        result["code"] = QStringLiteral("delete");
        result["detail"] = qs(error);
        return result;
    }
    refreshKeys();
    rebuild();
    emit metaChanged();
    emit changed();
    result["code"] = QString();
    result["files"] = names;
    return result;
}

QVariantMap SshBridge::deleteKnownHosts(const QVariantList& rows)
{
    QVariantMap result;
    std::vector<int> lines;
    for (const QVariant& row : rows) {
        const int index = row.toInt();
        if (index >= 0 && index < static_cast<int>(m_visibleKnownHosts.size()))
            lines.push_back(m_knownHosts[static_cast<size_t>(m_visibleKnownHosts[static_cast<size_t>(index)])].line);
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    if (lines.empty()) {
        result["code"] = QStringLiteral("empty");
        return result;
    }
    m_selectedKnownHosts = rows;
    if (m_knownHostsError.size()) {
        result["code"] = QStringLiteral("read");
        result["detail"] = m_knownHostsError;
        return result;
    }
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    const auto disk = store.readKnownHosts();
    if (!disk.ok) {
        result["code"] = QStringLiteral("read");
        result["detail"] = qs(disk.errMsg);
        return result;
    }
    if (disk.missing) {
        result["code"] = QStringLiteral("missing");
        return result;
    }
    if (disk.content != m_knownHostsText) {
        refreshKnownHosts();
        result["code"] = QStringLiteral("conflict");
        return result;
    }
    std::string updated;
    std::size_t pos = 0;
    int line = 0;
    while (pos < disk.content.size()) {
        const std::size_t nl = disk.content.find('\n', pos);
        const std::size_t end = nl == std::string::npos ? disk.content.size() : nl + 1;
        if (std::find(lines.begin(), lines.end(), line) == lines.end())
            updated.append(disk.content, pos, end - pos);
        pos = end;
        ++line;
    }
    envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
    const std::string backupId = backups.saveKnownHosts(disk.content);
    if (backupId.empty()) {
        result["code"] = QStringLiteral("backup");
        result["detail"] = QStringLiteral("cannot back up known_hosts");
        return result;
    }
    std::string error;
    if (!store.writeKnownHosts(updated, error)) {
        result["code"] = QStringLiteral("write");
        result["detail"] = qs(error);
        return result;
    }
    refreshKnownHosts();
    result["code"] = QString();
    result["backup"] = QString::fromStdString("known_hosts-" + backupId + ".known_hosts");
    return result;
}

void SshBridge::rebuild()
{
    const int prevConn = selectedConnIndex();

    QVector<SshConnModel::Row> rows;
    m_visibleConn.clear();
    const std::string needle = m_filter.trimmed().toStdString();
    std::vector<std::pair<int, int>> connMatches;
    for (int i = 0; i < static_cast<int>(m_cfg.connectionIndexes().size()); ++i) {
        const envh::ssh::Block* b = m_cfg.connection(i);
        if (!b)
            continue;
        const int score = envh::ssh::fuzzyScore(envh::ssh::displayName(*b) + " " + b->patterns + " " + b->fields.hostname + " "
                                                  + b->fields.user + " " + b->fields.port + " " + b->fields.identity + " "
                                                  + b->fields.proxyjump, needle);
        if (score > -1000000)
            connMatches.emplace_back(i, score);
    }
    std::stable_sort(connMatches.begin(), connMatches.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& match : connMatches) {
        const int i = match.first;
        m_visibleConn.push_back(i);
        const envh::ssh::Block* b = m_cfg.connection(i);
        if (!b)
            continue;
        SshConnModel::Row row;
        const bool locked = envh::ssh::isLocked(*b);
        if (b->type == envh::ssh::BlockType::Host) {
            const QString alias = qs(envh::ssh::trim(b->fields.alias));
            row.name = alias.isEmpty() ? (m_args.zh ? QStringLiteral("（新连接）") : QStringLiteral("(new connection)"))
                                       : alias + (locked ? (m_args.zh ? QStringLiteral("（全局）") : QStringLiteral(" (global)"))
                                                         : QString());
        } else {
            row.name = qs(envh::ssh::displayName(*b));
        }
        row.host = qs(b->fields.hostname);
        row.user = qs(b->fields.user);
        row.port = qs(b->fields.port);
        row.key = qs(b->fields.identity);
        row.locked = locked;
        row.deleted = b->deleted;
        row.isNew = b->isNew;
        row.mark = (b->dirty || b->isNew) ? QStringLiteral("●") : QString();
        row.conn = i;
        rows.append(row);
    }
    m_conns.resetRows(std::move(rows));

    if (prevConn >= 0) {
        int sel = -1;
        for (std::size_t i = 0; i < m_visibleConn.size(); ++i)
            if (m_visibleConn[i] == prevConn)
                sel = static_cast<int>(i);
        if (sel >= 0)
            m_connSel = sel;
    }
    if (m_visibleConn.empty())
        m_connSel = 0;
    else if (m_connSel >= static_cast<int>(m_visibleConn.size()))
        m_connSel = static_cast<int>(m_visibleConn.size()) - 1;

    const int prevKey = (m_keySel >= 0 && m_keySel < static_cast<int>(m_visibleKeys.size())) ? m_visibleKeys[static_cast<size_t>(m_keySel)] : -1;

    QVector<SshKeyModel::Row> krows;
    m_visibleKeys.clear();
    std::vector<std::pair<int, int>> keyMatches;
    for (int i = 0; i < static_cast<int>(m_keys.size()); ++i) {
        const envh::ssh::KeyFile& k = m_keys[static_cast<size_t>(i)];
        const int score = envh::ssh::fuzzyScore(k.name + " " + k.pub.type + " " + k.pub.comment + " " + k.pub.fingerprint, needle);
        if (score > -1000000)
            keyMatches.emplace_back(i, score);
    }
    std::stable_sort(keyMatches.begin(), keyMatches.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& match : keyMatches) {
        const int i = match.first;
        const envh::ssh::KeyFile& k = m_keys[static_cast<size_t>(i)];
        m_visibleKeys.push_back(i);
        SshKeyModel::Row row;
        row.name = qs(k.name);
        row.type = k.pub.ok ? qs(k.pub.type) : (m_args.zh ? QStringLiteral("未知") : QStringLiteral("unknown"));
        row.comment = qs(k.pub.comment);
        row.fingerprint = qs(k.pub.fingerprint);
        row.state = qs(envh::ssh::keyStateName(k.state));
        row.key = i;
        QStringList used;
        for (int c = 0; c < static_cast<int>(m_cfg.connectionIndexes().size()); ++c) {
            const envh::ssh::Block* b = m_cfg.connection(c);
            if (b && envh::ssh::usesKey(*b, k.name))
                used.append(qs(envh::ssh::displayName(*b)));
        }
        row.usedBy = used.join(QStringLiteral(", "));
        krows.append(row);
    }
    m_keyRows.resetRows(std::move(krows));

    if (prevKey >= 0) {
        for (std::size_t i = 0; i < m_visibleKeys.size(); ++i)
            if (m_visibleKeys[i] == prevKey)
                m_keySel = static_cast<int>(i);
    }
    if (m_visibleKeys.empty())
        m_keySel = 0;
    else if (m_keySel >= static_cast<int>(m_visibleKeys.size()))
        m_keySel = static_cast<int>(m_visibleKeys.size()) - 1;

    emit selectionChanged();
    emit detailChanged();
    emit previewChanged();
}

int SshBridge::selectedConnIndex() const
{
    if (m_connSel < 0 || m_connSel >= static_cast<int>(m_visibleConn.size()))
        return -1;
    return m_visibleConn[static_cast<size_t>(m_connSel)];
}

void SshBridge::selectConn(int row)
{
    if (row < 0 || row >= static_cast<int>(m_visibleConn.size()) || row == m_connSel)
        return;
    m_connSel = row;
    emit selectionChanged();
    emit detailChanged();
    emit previewChanged();
}

void SshBridge::selectKey(int row)
{
    if (row < 0 || row >= static_cast<int>(m_visibleKeys.size()) || row == m_keySel)
        return;
    m_keySel = row;
    emit selectionChanged();
    emit detailChanged();
}

void SshBridge::setConnSelection(const QVariantList& rows)
{
    m_selectedConns.clear();
    for (const QVariant& row : rows)
        m_selectedConns.append(row.toInt());
    emit previewChanged();
}

void SshBridge::setKeySelection(const QVariantList& rows)
{
    m_selectedKeys.clear();
    for (const QVariant& row : rows)
        m_selectedKeys.append(row.toInt());
}

void SshBridge::setFilter(const QString& text)
{
    if (text == m_filter)
        return;
    m_filter = text;
    m_selectedKnownHosts.clear();
    rebuild();
    emit filterChanged();
    emit changed();
}

void SshBridge::setKnownFilter(const QString& text)
{
    if (text == m_knownFilter)
        return;
    m_knownFilter = text;
    m_selectedKnownHosts.clear();
    refreshKnownHosts();
    emit knownFilterChanged();
}

QVariantMap SshBridge::connDetail() const
{
    QVariantMap m;
    const int ci = selectedConnIndex();
    if (ci < 0) {
        // Keep the shape stable so QML bindings never touch missing keys when
        // the config has no connections yet.
        m["canEdit"] = false;
        m["locked"] = false;
        m["deleted"] = false;
        m["isNew"] = false;
        m["fields"] = QVariantList {};
        m["extras"] = QVariantList {};
        m["fieldErrors"] = QVariantMap {};
        m["lockedNote"] = QString();
        m["name"] = QString();
        return m;
    }
    const envh::ssh::Block* b = m_cfg.connection(ci);
    if (!b)
        return m;

    const bool locked = envh::ssh::isLocked(*b);
    const std::map<std::string, std::string> errs = m_cfg.validate(ci);
    const auto err = [&errs](const char* key) {
        const auto it = errs.find(key);
        return it == errs.end() ? QString() : qs(it->second);
    };

    m["row"] = m_connSel;
    m["conn"] = ci;
    m["locked"] = locked;
    m["deleted"] = b->deleted;
    m["isNew"] = b->isNew;
    m["canEdit"] = !locked && !b->deleted;
    m["lockKind"] = b->type == envh::ssh::BlockType::Match ? QStringLiteral("match")
                    : b->type == envh::ssh::BlockType::Include ? QStringLiteral("include")
                    : locked ? QStringLiteral("global") : QString();
    QString name = qs(envh::ssh::displayName(*b));
    if (b->type == envh::ssh::BlockType::Host && b->isNew && envh::ssh::trim(b->fields.alias).empty())
        name = m_args.zh ? QStringLiteral("新连接") : QStringLiteral("New connection");
    m["name"] = name;
    m["alias"] = qs(b->fields.alias);
    m["hostname"] = qs(b->fields.hostname);
    m["user"] = qs(b->fields.user);
    m["port"] = qs(b->fields.port);
    m["identity"] = qs(b->fields.identity);
    m["proxyjump"] = qs(b->fields.proxyjump);

    QVariantList fields;
    const auto addField = [&](const char* key, QString label, QString value, QString placeholder, QString hint) {
        QVariantMap f;
        f["key"] = QString::fromLatin1(key);
        f["label"] = label;
        f["value"] = value;
        f["placeholder"] = placeholder;
        f["hint"] = hint;
        f["error"] = err(key);
        fields.append(f);
    };
    addField("alias", m_args.zh ? QStringLiteral("Host（别名）") : QStringLiteral("Host (alias)"), m["alias"].toString(),
             m_args.zh ? QStringLiteral("例如 work") : QStringLiteral("e.g. work"), QString());
    addField("hostname", QStringLiteral("HostName"), m["hostname"].toString(),
             m_args.zh ? QStringLiteral("例如 git.example.com") : QStringLiteral("e.g. git.example.com"), QString());
    addField("user", QStringLiteral("User"), m["user"].toString(),
             m_args.zh ? QStringLiteral("例如 git") : QStringLiteral("e.g. git"), QString());
    addField("port", QStringLiteral("Port"), m["port"].toString(), QStringLiteral("22"), QString());
    addField("identity", QStringLiteral("IdentityFile"), m["identity"].toString(),
             QStringLiteral("~/.ssh/id_ed25519"),
             m_args.zh ? QStringLiteral("输入时列出已检测到的密钥文件") : QStringLiteral("Lists the key files already detected"));
    addField("proxyjump", QStringLiteral("ProxyJump"), m["proxyjump"].toString(),
             m_args.zh ? QStringLiteral("例如 bastion") : QStringLiteral("e.g. bastion"),
             m_args.zh ? QStringLiteral("输入时列出其他连接别名") : QStringLiteral("Lists the other connection aliases"));
    m["fields"] = fields;

    QVariantList extras;
    for (const envh::ssh::Directive& d : b->directives) {
        const bool known = envh::ssh::isKnownHostKey(d.key);
        if (locked || !known) {
            QVariantMap e;
            e["key"] = qs(d.key);
            e["value"] = qs(d.value);
            e["line"] = qs(d.raw);
            extras.append(e);
        }
    }
    m["extras"] = extras;

    QVariantMap fieldErrors;
    for (const auto& p : errs)
        fieldErrors[qs(p.first)] = qs(p.second);
    m["fieldErrors"] = fieldErrors;

    if (locked) {
        m["lockedNote"] = m_args.zh
            ? QStringLiteral("以下块在本版本只读：保存时按原样保留，不在表单中展开。")
            : QStringLiteral("This block is read-only in this version: it is written back verbatim and never expanded into the form.");
    } else if (b->deleted) {
        m["lockedNote"] = m_args.zh ? QStringLiteral("已标记删除，保存后移除；再按一次删除键可恢复。")
                                    : QStringLiteral("Marked for deletion: removed on save. Press Delete again to restore.");
    } else {
        m["lockedNote"] = QString();
    }
    return m;
}

QVariantMap SshBridge::keyDetail() const
{
    QVariantMap m;
    if (m_keySel < 0 || m_keySel >= static_cast<int>(m_visibleKeys.size())) {
        m["hasPub"] = false;
        m["name"] = QString();
        m["usedBy"] = QStringList {};
        m["pubText"] = QString();
        return m;
    }
    const envh::ssh::KeyFile& k = m_keys[static_cast<size_t>(m_visibleKeys[static_cast<size_t>(m_keySel)])];
    m["name"] = qs(k.name);
    m["path"] = QDir::toNativeSeparators(QDir(m_dirText).filePath(qs(k.name)));
    m["type"] = qs(k.pub.type);
    m["bits"] = k.pub.bits;
    m["comment"] = qs(k.pub.comment);
    m["fingerprint"] = qs(k.pub.fingerprint);
    m["state"] = qs(envh::ssh::keyStateName(k.state));
    m["hasPub"] = k.pub.ok;
    QStringList used;
    for (int i = 0; i < static_cast<int>(m_cfg.connectionIndexes().size()); ++i) {
        const envh::ssh::Block* b = m_cfg.connection(i);
        if (b && envh::ssh::usesKey(*b, k.name))
            used.append(qs(envh::ssh::displayName(*b)));
    }
    m["usedBy"] = used;
    if (k.pub.ok) {
        QString line = qs(k.pub.algo + " " + envh::ssh::base64(k.pub.blob));
        if (!k.pub.comment.empty())
            line += QStringLiteral(" ") + qs(k.pub.comment);
        m["pubText"] = line;
    } else {
        m["pubText"] = QString();
    }
    return m;
}

QVariantList SshBridge::previewLines() const
{
    QVariantList out;
    for (const envh::ssh::Line& l : m_cfg.lines()) {
        QVariantMap row;
        row["text"] = qs(l.text);
        row["changed"] = l.changed;
        row["blank"] = l.blank;
        row["connection"] = l.connection;
        bool selected = false;
        if (l.connection >= 0) {
            for (const QVariant& selectedRow : m_selectedConns) {
                const int rowIndex = selectedRow.toInt();
                if (rowIndex >= 0 && rowIndex < static_cast<int>(m_visibleConn.size())
                    && m_visibleConn[static_cast<std::size_t>(rowIndex)] == l.connection) {
                    selected = true;
                    break;
                }
            }
        }
        row["selected"] = selected;
        out.append(row);
    }
    return out;
}

QVariantList SshBridge::changes() const
{
    QVariantList out;
    for (const envh::ssh::Change& c : m_cfg.changes()) {
        QVariantMap item;
        item["kind"] = c.kind == envh::ssh::ChangeKind::Add ? QStringLiteral("add")
                       : c.kind == envh::ssh::ChangeKind::Delete ? QStringLiteral("delete")
                                                                : QStringLiteral("modify");
        item["alias"] = qs(c.alias);
        QVariantList fields;
        for (const envh::ssh::FieldChange& f : c.fields) {
            QVariantMap field;
            field["key"] = qs(f.field);
            field["label"] = fieldLabel(f.field);
            field["from"] = qs(f.from);
            field["to"] = qs(f.to);
            fields.append(field);
        }
        item["fields"] = fields;
        out.append(item);
    }
    return out;
}

int SshBridge::addConnection()
{
    if (!m_loaded)
        return -1;
    const int conn = m_cfg.addConnection();
    if (!m_filter.isEmpty()) {
        m_filter.clear();
        emit filterChanged();
    }
    rebuild();
    for (std::size_t i = 0; i < m_visibleConn.size(); ++i)
        if (m_visibleConn[i] == conn) {
            m_connSel = static_cast<int>(i);
            break;
        }
    emit selectionChanged();
    emit detailChanged();
    emit changed();
    return m_connSel;
}

void SshBridge::createConfig()
{
    if (!m_loaded || !m_cfg.blocks().empty())
        return;
    m_cfg = envh::ssh::parseConfig("# ~/.ssh/config — managed by WinShun sshm\r\n"
                                   "# Add a connection with Ins; nothing is written until Ctrl+S.\r\n"
                                   "\r\n");
    addConnection();
    emit toast(QStringLiteral("created"), QString());
}

bool SshBridge::setConnField(const QString& field, const QString& value)
{
    const int ci = selectedConnIndex();
    if (ci < 0)
        return false;
    const envh::ssh::Block* b = m_cfg.connection(ci);
    if (!b || !envh::ssh::isEditable(*b) || b->deleted)
        return false;
    const std::string key = field.toStdString();
    const QString current = qs(key == "alias" ? b->fields.alias : key == "hostname" ? b->fields.hostname
                             : key == "user" ? b->fields.user : key == "port" ? b->fields.port
                             : key == "identity" ? b->fields.identity : key == "proxyjump" ? b->fields.proxyjump
                                                                                           : std::string());
    if (current == value)
        return true;
    m_cfg.beginEdit();
    if (!m_cfg.setField(ci, key, value.toStdString()))
        return false;
    rebuild();
    emit detailChanged();
    emit changed();
    return true;
}

void SshBridge::toggleDelete()
{
    const int ci = selectedConnIndex();
    if (ci < 0)
        return;
    if (!m_cfg.toggleDelete(ci))
        return;
    rebuild();
    emit detailChanged();
    emit changed();
    emit toast(QStringLiteral("deleted"), QString());
}

bool SshBridge::copyConnection()
{
    const int ci = selectedConnIndex();
    if (ci < 0)
        return false;
    const envh::ssh::Block* b = m_cfg.connection(ci);
    if (!b)
        return false;
    const QString text = qs(envh::ssh::serializeBlock(*b, m_cfg.eol())).trimmed();
    QGuiApplication::clipboard()->setText(text);
    emit toast(QStringLiteral("copiedRow"), QString());
    return true;
}

QVariantMap SshBridge::copySshCommand()
{
    const int ci = selectedConnIndex();
    if (ci < 0)
        return actionResult(QStringLiteral("noSelection"));
    const envh::ssh::Block* b = m_cfg.connection(ci);
    if (!b || b->type != envh::ssh::BlockType::Host || envh::ssh::isLocked(*b)
        || envh::ssh::trim(b->fields.alias).empty())
        return actionResult(QStringLiteral("unsupported"));
    const QString config = m_configText;
    const QString alias = qs(envh::ssh::trim(b->fields.alias));
    const QString command = QStringLiteral("ssh.exe -F \"") + config + QStringLiteral("\" \"") + alias + QStringLiteral("\"");
    QGuiApplication::clipboard()->setText(command);
    emit toast(QStringLiteral("copiedSshCommand"), command);
    return actionResult(QString(), QString(), command);
}

QVariantMap SshBridge::openWindowsTerminal()
{
    const int ci = selectedConnIndex();
    if (ci < 0)
        return actionResult(QStringLiteral("noSelection"));
    const envh::ssh::Block* b = m_cfg.connection(ci);
    if (!b || b->type != envh::ssh::BlockType::Host || envh::ssh::isLocked(*b)
        || envh::ssh::trim(b->fields.alias).empty())
        return actionResult(QStringLiteral("unsupported"));
    const std::wstring wt = envh::native::findProgram(L"wt.exe");
    if (wt.empty())
        return actionResult(QStringLiteral("notFound"), QStringLiteral("wt.exe"));
    const std::wstring ssh = envh::native::findProgram(L"ssh.exe");
    if (ssh.empty())
        return actionResult(QStringLiteral("notFound"), QStringLiteral("ssh.exe"));
    const std::wstring config = m_configText.toStdWString();
    const std::wstring alias = envh::native::toWide(envh::ssh::trim(b->fields.alias));
    const envh::native::LaunchResult launch = envh::native::launchProcess(
        wt, {L"new-tab", ssh, L"-F", config, alias});
    return launch.launched ? actionResult(QString()) : actionResult(QStringLiteral("launch"), qs(launch.err));
}

QVariantMap SshBridge::connectMotty()
{
    const int ci = selectedConnIndex();
    if (ci < 0)
        return actionResult(QStringLiteral("noSelection"));
    const envh::ssh::Block* b = m_cfg.connection(ci);
    if (!b || b->type != envh::ssh::BlockType::Host || envh::ssh::isLocked(*b)
        || envh::ssh::trim(b->fields.hostname).empty())
        return actionResult(QStringLiteral("unsupported"));
    const QString host = qs(envh::ssh::trim(b->fields.hostname));
    const QString user = qs(envh::ssh::trim(b->fields.user));
    const QString portText = qs(envh::ssh::trim(b->fields.port));
    bool portOk = false;
    const int port = portText.isEmpty() ? 22 : portText.toInt(&portOk);
    if (!portOk && !portText.isEmpty())
        return actionResult(QStringLiteral("invalid"));
    QUrl url;
    url.setScheme(QStringLiteral("ssh"));
    if (!user.isEmpty())
        url.setUserName(user);
    url.setHost(host);
    url.setPort(port);
    const QString encoded = QString::fromUtf8(url.toEncoded());
    std::string error;
    if (!envh::native::openUrl(encoded.toStdWString(), error))
        return actionResult(QStringLiteral("launch"), qs(error));
    return actionResult(QString(), QString(), encoded);
}

bool SshBridge::copyPublicKey()
{
    if (m_keySel < 0 || m_keySel >= static_cast<int>(m_visibleKeys.size()))
        return false;
    const envh::ssh::KeyFile& k = m_keys[static_cast<size_t>(m_visibleKeys[static_cast<size_t>(m_keySel)])];
    if (!k.pub.ok)
        return false;
    QString line = qs(k.pub.algo + " " + envh::ssh::base64(k.pub.blob));
    if (!k.pub.comment.empty())
        line += QStringLiteral(" ") + qs(k.pub.comment);
    QGuiApplication::clipboard()->setText(line);
    emit toast(QStringLiteral("copiedPub"), QString());
    return true;
}

void SshBridge::undo()
{
    if (!m_cfg.undo())
        return;
    rebuild();
    emit detailChanged();
    emit changed();
}

void SshBridge::redo()
{
    if (!m_cfg.redo())
        return;
    rebuild();
    emit detailChanged();
    emit changed();
}

QVariantMap SshBridge::saveImpl(bool force)
{
    QVariantMap res;
    if (m_cfg.anyInvalid()) {
        res["code"] = QStringLiteral("invalid");
        return res;
    }
    if (!m_cfg.dirty()) {
        res["code"] = QStringLiteral("clean");
        return res;
    }
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    const envh::native::SshReadResult disk = store.read();
    if (!disk.ok) {
        res["code"] = QStringLiteral("load");
        res["detail"] = qs(disk.errMsg);
        return res;
    }
    const QString diskText = qs(disk.content);
    const QString localText = qs(m_cfg.serialize());
    if (!force && diskText != qs(m_cfg.base())) {
        m_conflictDiff = diffLists(displayLines(diskText), displayLines(localText));
        m_conflictVisible = true;
        emit conflictChanged();
        res["code"] = QStringLiteral("conflict");
        return res;
    }

    envh::native::BackupStore backups(m_args.dataDir, m_args.backupCount);
    QString backupId;
    if (!disk.missing)
        backupId = qs(backups.saveSsh(disk.content));
    std::string err;
    if (!store.write(m_cfg.serialize(), err)) {
        res["code"] = QStringLiteral("write");
        res["detail"] = qs(err);
        return res;
    }
    m_cfg.reload(m_cfg.serialize());
    m_hasConfig = true;
    m_conflictVisible = false;
    m_conflictDiff.clear();
    refreshKeys();
    rebuild();
    emit metaChanged();
    emit changed();
    emit conflictChanged();
    emit toast(QStringLiteral("saved"), backupId);
    res["code"] = QString();
    res["id"] = backupId;
    return res;
}

QVariantMap SshBridge::save()
{
    return saveImpl(false);
}

QVariantMap SshBridge::saveForce()
{
    return saveImpl(true);
}

QVariantMap SshBridge::reloadFromDisk()
{
    QVariantMap res;
    if (!m_loaded) {
        res["code"] = QStringLiteral("load");
        res["detail"] = m_loadError;
        return res;
    }
    reread(true);
    if (!m_loadError.isEmpty()) {
        res["code"] = QStringLiteral("load");
        res["detail"] = m_loadError;
        return res;
    }
    emit toast(QStringLiteral("reloaded"), QString());
    res["code"] = QString();
    return res;
}

void SshBridge::closeConflict()
{
    if (!m_conflictVisible)
        return;
    m_conflictVisible = false;
    m_conflictDiff.clear();
    emit conflictChanged();
}

QVariantMap SshBridge::generateKey(const QString& name, const QString& type, const QString& comment,
                                   const QString& passphrase)
{
    QVariantMap res;
    m_keygenDetail.clear();
    if (!m_keygenAvailable) {
        res["code"] = QStringLiteral("E_NOTOOL");
        return res;
    }
    envh::ssh::KeygenSpec spec;
    spec.fileName = name.trimmed().toStdString();
    spec.type = type.toStdString();
    spec.comment = comment.trimmed().toStdString();
    spec.passphrase = passphrase.toStdString();
    envh::native::SshStore store(m_args.sshDir, m_dirText.toStdWString() + L"\\config");
    const envh::ssh::Check check = envh::ssh::validateKeygen(spec, store.fileNames());
    if (!check.ok) {
        m_keygenDetail = qs(check.detail);
        emit keygenFinished();
        res["code"] = qs(check.code);
        res["detail"] = m_keygenDetail;
        return res;
    }
    const std::wstring tool = envh::native::findSshKeygen();
    if (tool.empty()) {
        m_keygenAvailable = false;
        emit metaChanged();
        res["code"] = QStringLiteral("E_NOTOOL");
        return res;
    }
    store.ensureDir();
    const std::wstring keyPath = m_args.sshDir + L"\\" + envh::native::toWide(spec.fileName);
    std::vector<std::wstring> args;
    for (const std::string& a : envh::ssh::keygenArgs(spec, envh::native::fromWide(keyPath)))
        args.push_back(envh::native::toWide(a));
    const envh::native::ProcResult proc = envh::native::runProcess(tool, args);
    if (!proc.launched || !proc.ok) {
        m_keygenDetail = capped(proc.err.empty() ? proc.out : proc.err);
        if (m_keygenDetail.isEmpty())
            m_keygenDetail = proc.launched ? QStringLiteral("ssh-keygen exited with code ") + QString::number(proc.exitCode)
                                           : qs(proc.err);
        emit keygenFinished();
        res["code"] = QStringLiteral("E_FAILED");
        res["detail"] = m_keygenDetail;
        return res;
    }
    refreshKeys();
    rebuild();
    emit metaChanged();
    emit changed();
    emit keygenFinished();
    emit toast(QStringLiteral("keygen"), qs(spec.fileName));
    res["code"] = QString();
    res["detail"] = qs(spec.fileName);
    return res;
}

} // namespace editorwin
