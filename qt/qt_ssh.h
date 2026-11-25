// qt_ssh.h - Qt Quick front-end for the SSH connections & keys window.
//
// Same split as the rest of the plugin: ssh_model.h owns the rules (parsing,
// byte-exact writing, change list, key parsing), native.h owns the files and
// ssh-keygen, and this bridge only translates both into a shape QML can bind.
// The window is read/write for connections and read-only for keys, except that
// it can ask ssh-keygen to create a new key pair.
//
// Private key files are classified by their header in native.h and their body
// never reaches this class: key rows and details carry public keys only.
//
// Code and comments are English.
#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QVariantList>
#include <QHash>
#include <QVariantMap>
#include <QVector>

#include <string>
#include <vector>

#include "editor_common.h"
#include "ssh_model.h"

namespace editorwin {

class SshConnModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        HostRole,
        UserRole,
        PortRole,
        KeyRole,
        MarkRole,
        LockedRole,
        DeletedRole,
        NewRole,
        ConnRole,
    };

    struct Row {
        QString name;
        QString host;
        QString user;
        QString port;
        QString key;
        QString mark;
        bool locked = false;
        bool deleted = false;
        bool isNew = false;
        int conn = -1; // index into Config::connectionIndexes()
    };

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void resetRows(QVector<Row> rows);

private:
    QVector<Row> m_rows;
};

class SshKeyModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        TypeRole,
        CommentRole,
        FingerprintRole,
        StateRole,
        UsedByRole,
        KeyRole,
    };

    struct Row {
        QString name;
        QString type;
        QString comment;
        QString fingerprint;
        QString state;
        QString usedBy;
        int key = -1; // index into the bridge's key list
    };

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void resetRows(QVector<Row> rows);

private:
    QVector<Row> m_rows;
};

class SshBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY metaChanged)
    Q_PROPERTY(QString sshDir READ sshDir NOTIFY metaChanged)
    Q_PROPERTY(QString configPath READ configPath NOTIFY metaChanged)
    Q_PROPERTY(QString backupDir READ backupDir NOTIFY metaChanged)
    Q_PROPERTY(bool zh READ zh NOTIFY metaChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY metaChanged)
    Q_PROPERTY(QVariantMap appearance READ appearance NOTIFY metaChanged)
    Q_PROPERTY(bool hasConfig READ hasConfig NOTIFY metaChanged)
    Q_PROPERTY(bool hasLocked READ hasLocked NOTIFY metaChanged)
    Q_PROPERTY(QString startTab READ startTab CONSTANT)
    Q_PROPERTY(bool emptyState READ emptyState NOTIFY metaChanged)
    Q_PROPERTY(QString loadError READ loadError NOTIFY metaChanged)
    Q_PROPERTY(bool keygenAvailable READ keygenAvailable NOTIFY metaChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(int dirtyCount READ dirtyCount NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool anyInvalid READ anyInvalid NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString knownFilter READ knownFilter WRITE setKnownFilter NOTIFY knownFilterChanged)
    Q_PROPERTY(SshConnModel* conns READ conns CONSTANT)
    Q_PROPERTY(SshKeyModel* keys READ keys CONSTANT)
    Q_PROPERTY(QVariantList knownHosts READ knownHosts NOTIFY knownHostsChanged)
    Q_PROPERTY(QString knownHostsError READ knownHostsError NOTIFY knownHostsChanged)
    Q_PROPERTY(QVariantList agentKeys READ agentKeys NOTIFY agentChanged)
    Q_PROPERTY(QString agentError READ agentError NOTIFY agentChanged)
    Q_PROPERTY(QString agentSource READ agentSource CONSTANT)
    Q_PROPERTY(QString agentSourceLabel READ agentSourceLabel CONSTANT)
    Q_PROPERTY(QStringList privateKeyNames READ privateKeyNames NOTIFY metaChanged)
    Q_PROPERTY(int connSel READ connSel NOTIFY selectionChanged)
    Q_PROPERTY(int keySel READ keySel NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap connDetail READ connDetail NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap keyDetail READ keyDetail NOTIFY detailChanged)
    Q_PROPERTY(QVariantList previewLines READ previewLines NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap connectionTest READ connectionTest NOTIFY connectionTestChanged)
    Q_PROPERTY(bool testing READ testing NOTIFY connectionTestChanged)
    Q_PROPERTY(QVariantList changes READ changes NOTIFY changed)
    Q_PROPERTY(QVariantList conflictDiff READ conflictDiff NOTIFY conflictChanged)
    Q_PROPERTY(bool conflictVisible READ conflictVisible NOTIFY conflictChanged)
    Q_PROPERTY(QString keygenDetail READ keygenDetail NOTIFY keygenFinished)

public:
    SshBridge(OpenArgs args, QObject* parent = nullptr);

    QString title() const;
    QString sshDir() const { return m_dirText; }
    QString configPath() const { return m_configText; }
    QString backupDir() const;
    bool zh() const { return m_args.zh; }
    bool dark() const { return m_args.dark; }
    QVariantMap appearance() const { return m_appearance; }
    bool hasConfig() const { return m_hasConfig; }
    bool hasLocked() const;
    QString startTab() const { return m_args.select.rfind("key:", 0) == 0 ? QStringLiteral("keys") : QStringLiteral("conn"); }
    bool emptyState() const;
    QString loadError() const { return m_loadError; }
    bool keygenAvailable() const { return m_keygenAvailable; }
    bool dirty() const { return m_cfg.dirty(); }
    int dirtyCount() const { return m_cfg.dirtyCount(); }
    bool canUndo() const { return m_cfg.canUndo(); }
    bool canRedo() const { return m_cfg.canRedo(); }
    bool anyInvalid() const { return m_cfg.anyInvalid(); }
    bool canSave() const { return m_cfg.dirty() && !m_cfg.anyInvalid(); }
    QString filter() const { return m_filter; }
    void setFilter(const QString& text);
    QString knownFilter() const { return m_knownFilter; }
    void setKnownFilter(const QString& text);
    SshConnModel* conns() { return &m_conns; }
    SshKeyModel* keys() { return &m_keyRows; }
    int connSel() const { return m_connSel; }
    int keySel() const { return m_keySel; }
    QVariantMap connDetail() const;
    QVariantMap keyDetail() const;
    QVariantList previewLines() const;
    QVariantMap connectionTest() const { return m_connectionTest; }
    bool testing() const { return m_testProcess != nullptr; }
    QVariantList changes() const;
    QVariantList knownHosts() const;
    QString knownHostsError() const { return m_knownHostsError; }
    QVariantList agentKeys() const { return m_agentKeys; }
    QString agentError() const { return m_agentError; }
    QString agentSource() const { return m_args.sshAgent.empty() ? QStringLiteral("auto") : QString::fromStdString(m_args.sshAgent); }
    QString agentSourceLabel() const;
    QStringList privateKeyNames() const;
    QVariantList conflictDiff() const { return m_conflictDiff; }
    bool conflictVisible() const { return m_conflictVisible; }
    QString keygenDetail() const { return m_keygenDetail; }

    bool load(QString& error);
    void applyFocus(const OpenArgs& a);
    void reload();
    void setAppearance(envh::i18n::Lang lang, bool dark, const std::string& appearanceJson = {});

    Q_INVOKABLE void selectConn(int row);
    Q_INVOKABLE void selectKey(int row);
    Q_INVOKABLE void setConnSelection(const QVariantList& rows);
    Q_INVOKABLE void setKeySelection(const QVariantList& rows);
    Q_INVOKABLE int deleteConnections(const QVariantList& rows);
    Q_INVOKABLE QStringList keyFiles(const QVariantList& rows) const;
    Q_INVOKABLE QVariantMap deleteKeys(const QVariantList& rows);
    Q_INVOKABLE QVariantMap deleteKnownHosts(const QVariantList& rows);
    Q_INVOKABLE QVariantMap refreshAgent();
    Q_INVOKABLE QVariantMap addAgentKey(const QString& name);
    Q_INVOKABLE QVariantMap addAgentKeyPath(const QString& path);
    Q_INVOKABLE QVariantMap browseAgentKey();
    Q_INVOKABLE QVariantMap removeAgentKeys(const QVariantList& fingerprints);
    Q_INVOKABLE QVariantMap clearAgent();
    Q_INVOKABLE void reloadKnownHosts() { refreshKnownHosts(); }
    Q_INVOKABLE int addConnection();
    Q_INVOKABLE void createConfig();
    Q_INVOKABLE bool setConnField(const QString& field, const QString& value);
    Q_INVOKABLE void toggleDelete();
    Q_INVOKABLE bool copyConnection();
    Q_INVOKABLE QVariantMap copySshCommand();
    Q_INVOKABLE QVariantMap openWindowsTerminal();
    Q_INVOKABLE QVariantMap connectSshUrl();
    Q_INVOKABLE QVariantMap startConnectionTest();
    Q_INVOKABLE void cancelConnectionTest();
    Q_INVOKABLE bool copyPublicKey();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE QVariantMap save();
    Q_INVOKABLE QVariantMap saveForce();
    Q_INVOKABLE QVariantMap reloadFromDisk();
    Q_INVOKABLE void closeConflict();
    Q_INVOKABLE QVariantMap generateKey(const QString& name, const QString& type, const QString& comment,
                                        const QString& passphrase);

signals:
    void metaChanged();
    void changed();
    void filterChanged();
    void knownFilterChanged();
    void selectionChanged();
    void detailChanged();
    void previewChanged();
    void conflictChanged();
    void keygenFinished();
    void knownHostsChanged();
    void agentChanged();
    void connectionTestChanged();
    void toast(const QString& code, const QString& detail);

private:
    void parseAppearance(const std::string& appearanceJson);
    int selectedConnIndex() const;
    void rebuild();
    void refreshKeys();
    void refreshKnownHosts();
    void reread(bool keepSelection);
    void refreshDetail();
    QVariantMap saveImpl(bool force);
    bool writeContent(const QString& content, const QString& diskText, bool backup, QString& error, QString& id);

    OpenArgs m_args;
    QVariantMap m_appearance;
    envh::ssh::Config m_cfg;
    std::vector<envh::ssh::KeyFile> m_keys;
    SshConnModel m_conns;
    SshKeyModel m_keyRows;
    QString m_filter;
    QString m_knownFilter;
    QString m_dirText;
    QString m_configText;
    QString m_loadError;
    QString m_keygenDetail;
    QString m_knownHostsError;
    QString m_agentError;
    std::string m_knownHostsText;
    std::vector<envh::ssh::KnownHost> m_knownHosts;
    std::vector<int> m_visibleKnownHosts;
    QVariantList m_selectedKnownHosts;
    QVariantList m_selectedConns;
    QVariantList m_selectedKeys;
    QVariantList m_agentKeys;
    QHash<QString, QString> m_agentExternalPaths;
    QString m_pendingAgentPath;
    QVariantList m_conflictDiff;
    QVariantMap m_connectionTest {{QStringLiteral("state"), QStringLiteral("idle")}};
    QProcess* m_testProcess = nullptr;
    QElapsedTimer m_testTimer;
    bool m_hasConfig = false;
    bool m_keygenAvailable = false;
    bool m_conflictVisible = false;
    bool m_loaded = false;
    int m_connSel = 0;
    int m_keySel = 0;
    std::vector<int> m_visibleConn;
    std::vector<int> m_visibleKeys;
};

} // namespace editorwin
