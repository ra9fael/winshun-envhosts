#pragma once
#include "editor_session.h"
#include "editor_common.h"
#include <QMap>
#include <memory>

namespace editorwin {

struct WriteRequest {
    QString kind, revision, token;
    QMap<QString,QString> fingerprints;
    QVector<Change> changes;
    QByteArray hosts;
};
struct WriteResult {
    QString state="Rejected", error, backupId;
    QStringList completed, failed, notExecuted;
    TargetSnapshot actual;
};
class StoreBackend {
public:
    virtual ~StoreBackend()=default;
    virtual TargetSnapshot read()=0;
    virtual bool backup(const TargetSnapshot&,const QStringList& scopes,QString& id,QString& error)=0;
    virtual bool writeEnv(const Change&,QString& error)=0;
    virtual bool writeHosts(const QByteArray&,QString& error)=0;
    virtual bool broadcast()=0;
};
class WriteService {
public:
    static WriteResult execute(StoreBackend& store,const WriteRequest& request);
};
struct BackupRecord {QString id,scope; qint64 bytes=0;};
class NativeBackend : public StoreBackend {
public:
    explicit NativeBackend(OpenArgs args) : args(std::move(args)) {}
    OpenArgs args;
    QString identity() const;
    TargetSnapshot read() override;
    bool backup(const TargetSnapshot&,const QStringList&,QString&,QString&) override;
    bool writeEnv(const Change&,QString&) override;
    bool writeHosts(const QByteArray&,QString&) override;
    bool broadcast() override;
    QVector<BackupRecord> backups(QString& error) const;
    bool readBackup(const QString& id,TargetSnapshot& snapshot,QString& scope,QString& error) const;
    bool removeBackup(const QString& id,QString& error) const;
    QString exportBackup(const QString& id,QString& error) const;
    bool saveDraft(const QJsonObject&,QString& error) const;
    bool saveRecoveryDraft(const QJsonObject&,QString& error) const;
    QJsonObject readDraft(QString& error) const;
    bool removeDraft(QString& error) const;
private:
    QString backupDir() const;
    QString backupPath(const QString& id) const;
    QString draftPath() const;
};

} // namespace editorwin
