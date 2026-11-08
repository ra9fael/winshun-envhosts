#pragma once
#include "editor_types.h"
#include "editor_model.h"
#include <QHash>
#include <functional>

namespace editorwin {

struct Part {
    QString id, value;
    bool isNew=false, deleted=false;
};
struct PartsState {
    QString delimiter=";";
    QVector<Part> items;
};
struct WorkingState {
    QVector<EnvValue> env;
    envh::edit::HostsModel hosts;
    QStringList hostIds;
    QHash<QString,PartsState> parts;
};
struct EditBuffer {
    QString token, row, part, field, before, text;
    bool composing=false;
    bool pending() const { return !token.isEmpty() && before != text; }
};

class EditorSession {
public:
    explicit EditorSession(QString kind="env") : kind(std::move(kind)) {}
    QString kind;
    TargetSnapshot base;
    WorkingState work;
    Selection rows;
    QHash<QString,Selection> partSelections;
    EditBuffer buffer;
    quint64 revision=0;
    QString error;
    bool writing=false;
    bool baselineUnknown=false;

    void load(const TargetSnapshot& snapshot, bool preserveIds=false);
    TargetSnapshot effective() const;
    bool dirty() const;
    QStringList visible(const QString& filter={},const QString& scope="all",bool showSystem=true) const;
    EnvValue* env(const QString& id);
    const EnvValue* env(const QString& id) const;
    int hostIndex(const QString& id) const;
    bool writable(const QString& id) const;
    bool writable(const EnvValue& value) const;
    PartsState* ensureParts(const QString& row);
    QStringList partIds(const QString& row) const;
    QString rawText() const;
    QString field(const QString& row,const QString& name,const QString& part={}) const;
    QString beginEdit(const QString& row,const QString& field,const QString& part={});
    void updateEdit(const QString& token,const QString& text,bool composing=false);
    bool finishEdit();
    void cancelEdit();
    bool editField(const QString& row,const QString& field,const QString& value,const QString& part={});
    QString addRow(const QString& scope="user",const QStringList& visibleOrder={});
    int deleteRows(const QStringList& ids,bool deleted);
    int enableRows(const QStringList& ids,bool enabled);
    QString addPart(const QString& row);
    QString duplicateParts(const QString& row);
    bool deleteParts(const QString& row,const QStringList& ids,bool deleted);
    bool moveParts(const QString& row,const QStringList& ids,int direction);
    bool delimiter(const QString& row,const QString& value,bool confirmed=false);
    bool canUndo() const { return !undoStates.isEmpty(); }
    bool canRedo() const { return !redoStates.isEmpty(); }
    bool history(bool redo);
    QVector<Change> changes() const;
    QStringList validation() const;
    QStringList warnings() const;
    bool restore(const TargetSnapshot& snapshot,const QString& scope);
    QJsonObject draft(const QString& identity) const;
    bool restoreDraft(const QJsonObject& object,const QString& identity);
    int undoCount() const { return int(undoStates.size()); }
private:
    QVector<WorkingState> undoStates,redoStates;
    bool mutate(const std::function<void()>& fn);
    QByteArray stateBytes() const;
    void syncParts(const QString& row);
    void buildHosts(const QByteArray& bytes,bool keepIds=false);
    bool applyRaw(const QString& text);
};

} // namespace editorwin
